// Stratum — what a Stratum world's generator options say.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

import java.io.File;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * A Stratum world's generator options: the string Nukkit stores as
 * {@code generatorOptions} in {@code level.dat} and hands every generator it
 * constructs as {@code options.get("preset")} — from {@code server.properties}'
 * {@code generator-settings} for the default world, or from the text after
 * {@code stratum:} in {@code nukkit.yml}'s {@code worlds.<name>.generator}.
 * It is the only configuration a generator receives, on every thread, so
 * the path to the world's frozen pipeline (SPEC §6) travels in it.
 *
 * <pre>
 *   {"blob":"worlds/stratum/stratum-pipeline.blob",
 *    "pack":"/path/to/.fixtures/1.21.11",
 *    "settings":"minecraft:overworld",
 *    "biomes":"minecraft:overworld"}
 * </pre>
 *
 * <p>The same keys as {@code ext/}'s PocketMine-MP options, plus
 * {@code "pack"}, and the same rules:
 * <ul>
 * <li>{@code "blob"}, required: the frozen pipeline the world generates
 * from. A relative path is resolved against the server's working directory,
 * as Nukkit resolves its own {@code worlds/}.
 * <li>{@code "pack"}, optional: a version root ({@code worldgen/} and
 * {@code biome_parameters/} side by side, as {@code tools/fetch-vanilla}
 * leaves them). Read once, by the plugin, to write {@code "blob"} before
 * the world is created, and never after: a world that already exists
 * without its blob is refused, not refrozen.
 * <li>{@code "settings"} and {@code "biomes"}: the noise settings and the
 * biome source, together or not at all; with neither, both are the
 * overworld's. Either alone would quietly pair with the overworld's other
 * half (SPEC §8).
 * </ul>
 * A JSON object of strings and nothing else: another key, a repeated key, a
 * value that is not a string (null included) or trailing text is refused,
 * naming it, rather than ignored.
 */
public final class GeneratorOptions {
    public static final String DEFAULT_DIMENSION = "minecraft:overworld";

    public final String blob;
    public final String pack;
    public final String settings;
    public final String biomes;

    private GeneratorOptions(String blob, String pack, String settings, String biomes) {
        this.blob = blob;
        this.pack = pack;
        this.settings = settings;
        this.biomes = biomes;
    }

    /** {@link #blob} as an absolute file, relative paths against the working directory. */
    public File blobFile() {
        return new File(blob).getAbsoluteFile();
    }

    public static GeneratorOptions parse(String text) {
        if (text == null || text.trim().isEmpty()) {
            throw new StratumGenerationException("stratum: a Stratum world needs generator options "
                + "naming its frozen pipeline, e.g. {\"blob\":\"worlds/<name>/stratum-pipeline.blob\","
                + "\"pack\":\"<version root>\"}; it has none");
        }
        final Map<String, String> values = new Reader(text).readObject();
        for (String key : values.keySet()) {
            if (!key.equals("blob") && !key.equals("pack") && !key.equals("settings")
                && !key.equals("biomes")) {
                throw new StratumGenerationException("stratum: generator options take \"blob\", "
                    + "\"pack\", \"settings\" and \"biomes\" and nothing else, so \"" + key
                    + "\" would be ignored: " + text);
            }
        }
        final String blob = values.get("blob");
        if (blob == null || blob.isEmpty()) {
            throw new StratumGenerationException("stratum: generator options have no \"blob\" path "
                + "to a frozen pipeline: " + text);
        }
        final String pack = values.get("pack");
        if (pack != null && pack.isEmpty()) {
            throw new StratumGenerationException("stratum: generator options name an empty \"pack\": "
                + text);
        }
        final boolean hasSettings = values.containsKey("settings");
        if (hasSettings != values.containsKey("biomes")) {
            throw new StratumGenerationException(hasSettings
                ? "stratum: generator options name \"settings\" without \"biomes\", the biome "
                    + "source to go with them: " + text
                : "stratum: generator options name \"biomes\" without \"settings\", the noise "
                    + "settings to go with them: " + text);
        }
        return new GeneratorOptions(blob, pack,
            hasSettings ? values.get("settings") : DEFAULT_DIMENSION,
            hasSettings ? values.get("biomes") : DEFAULT_DIMENSION);
    }

    /** The options string for these values, as {@link #parse} reads it back. */
    public static String encode(String blob, String pack, String settings, String biomes) {
        if ((settings == null) != (biomes == null)) {
            throw new IllegalArgumentException("settings and biomes come together or not at all");
        }
        final StringBuilder text = new StringBuilder("{");
        appendPair(text, "blob", blob);
        if (pack != null) {
            appendPair(text.append(','), "pack", pack);
        }
        if (settings != null) {
            appendPair(text.append(','), "settings", settings);
            appendPair(text.append(','), "biomes", biomes);
        }
        return text.append('}').toString();
    }

    private static void appendPair(StringBuilder text, String key, String value) {
        appendString(text, key);
        text.append(':');
        appendString(text, value);
    }

    private static void appendString(StringBuilder text, String value) {
        text.append('"');
        for (int i = 0; i < value.length(); i++) {
            final char c = value.charAt(i);
            if (c == '"' || c == '\\') {
                text.append('\\').append(c);
            } else if (c < 0x20) {
                text.append(String.format("\\u%04x", (int) c));
            } else {
                text.append(c);
            }
        }
        text.append('"');
    }

    /** A strict reader for one flat JSON object of string values. */
    private static final class Reader {
        private final String text;
        private int at;

        Reader(String text) {
            this.text = text;
        }

        Map<String, String> readObject() {
            final Map<String, String> values = new LinkedHashMap<>();
            skipSpace();
            expect('{');
            skipSpace();
            if (peek() == '}') {
                at++;
            } else {
                while (true) {
                    skipSpace();
                    final String key = readString();
                    skipSpace();
                    expect(':');
                    skipSpace();
                    if (peek() != '"') {
                        throw refuse("the value of \"" + key + "\" is not a string");
                    }
                    final String value = readString();
                    if (values.put(key, value) != null) {
                        throw refuse("\"" + key + "\" is given twice");
                    }
                    skipSpace();
                    if (peek() == ',') {
                        at++;
                        continue;
                    }
                    expect('}');
                    break;
                }
            }
            skipSpace();
            if (at != text.length()) {
                throw refuse("there is text after the closing brace");
            }
            return values;
        }

        private String readString() {
            expect('"');
            final StringBuilder value = new StringBuilder();
            while (true) {
                if (at >= text.length()) {
                    throw refuse("a string is not closed");
                }
                final char c = text.charAt(at++);
                if (c == '"') {
                    return value.toString();
                }
                if (c < 0x20) {
                    throw refuse("a string holds a raw control character");
                }
                if (c != '\\') {
                    value.append(c);
                    continue;
                }
                if (at >= text.length()) {
                    throw refuse("a string ends in a lone backslash");
                }
                final char escaped = text.charAt(at++);
                switch (escaped) {
                    case '"':
                    case '\\':
                    case '/':
                        value.append(escaped);
                        break;
                    case 'b':
                        value.append('\b');
                        break;
                    case 'f':
                        value.append('\f');
                        break;
                    case 'n':
                        value.append('\n');
                        break;
                    case 'r':
                        value.append('\r');
                        break;
                    case 't':
                        value.append('\t');
                        break;
                    case 'u':
                        if (at + 4 > text.length()) {
                            throw refuse("a \\u escape is cut short");
                        }
                        try {
                            value.append((char) Integer.parseInt(text.substring(at, at + 4), 16));
                        } catch (NumberFormatException e) {
                            throw refuse("a \\u escape is not four hex digits");
                        }
                        at += 4;
                        break;
                    default:
                        throw refuse("\\" + escaped + " is not a JSON escape");
                }
            }
        }

        private char peek() {
            if (at >= text.length()) {
                throw refuse("it ends early");
            }
            return text.charAt(at);
        }

        private void expect(char wanted) {
            if (peek() != wanted) {
                throw refuse("expected '" + wanted + "' at offset " + at);
            }
            at++;
        }

        private void skipSpace() {
            while (at < text.length() && Character.isWhitespace(text.charAt(at))) {
                at++;
            }
        }

        private StratumGenerationException refuse(String why) {
            return new StratumGenerationException("stratum: generator options are not a JSON object "
                + "of strings (" + why + "): " + text);
        }
    }
}
