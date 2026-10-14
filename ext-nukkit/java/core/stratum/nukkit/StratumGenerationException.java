// Stratum — the exception every refusal of the Nukkit binding raises.
// Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0
package stratum.nukkit;

/**
 * Raised for anything the binding refuses rather than guesses at (SPEC §8):
 * a world it cannot open, a block state Nukkit cannot place, generator
 * options it cannot read. The native library raises it too, for every C++
 * exception, so a refusal reaches Java as this type and never as a crash.
 * Unchecked because Nukkit's {@code Generator} methods declare no checked
 * exceptions.
 */
public class StratumGenerationException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    public StratumGenerationException(String message) {
        super(message);
    }

    public StratumGenerationException(String message, Throwable cause) {
        super(message, cause);
    }
}
