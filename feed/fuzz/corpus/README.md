# Fuzzer seed corpus

Small, **valid** ITCH 5.0 streams in BinaryFILE framing. They are committed
(a few hundred bytes in total) because a fuzzer started from nothing spends
its first minutes rediscovering the 2-byte length prefix, and seeds that
already reach the decoder's interesting branches make the same wall-clock
budget worth far more.

| File | What it reaches |
|---|---|
| `open_and_adds.bin` | system events and the plain add path |
| `full_lifecycle.bin` | add, execute, partial cancel, replace, delete on one order |
| `executions.bin` | `E`, and `C` with both printable and non-printable |
| `prints_and_cross.bin` | hidden `P` prints, opening and closing crosses, a broken trade |
| `every_book_type.bin` | all seven book-mutating types in one stream |
| `single_add.bin` | the smallest interesting input |

Regenerate or extend them with `feed/tests/itch_encoder.hpp`; there is nothing
magic about these particular bytes.
