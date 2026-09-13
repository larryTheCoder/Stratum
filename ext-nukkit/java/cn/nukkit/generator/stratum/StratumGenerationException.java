package cn.nukkit.generator.stratum;

/**
 * Thrown by {@link StratumGenerator}'s native methods for anything
 * {@code stratum_nukkit::Pipeline} refuses rather than guesses at (SPEC
 * section 8 of the Stratum engine this wraps) — an unsupported dimension,
 * a block state this build cannot yet place into a Nukkit chunk, or a
 * malformed datapack. A {@code RuntimeException} rather than a checked one:
 * {@link cn.nukkit.level.generator.Generator}'s own abstract methods declare
 * no checked exceptions, so this could not be a checked type without
 * breaking the interface it implements.
 */
public class StratumGenerationException extends RuntimeException {
    public StratumGenerationException(String message) {
        super(message);
    }
}
