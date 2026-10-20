# Dungeons & Dragons: Shadow over Mystara

ROM set `ddsom` (Europe 960619): eight program files, eight graphics files, `dd2.01` (sound CPU), two sample files
and `ddsom.key`. `dd2.02` is part of the set and is not read.

## State

- Program deciphered from the key, translated to C++ (`work/ddsom/gen/`) and built; the sound CPU's program (QSound
  driver 1.15) translated too. After `explore`, a 20000-frame made-up-play run executes 0 untranslated instructions.
- `make GAME=ddsom explore` finds more of the code by made-up play; rerun it after changing anything in `game.cfg`.
- No game-specific sources yet: `src/` is empty, so none of the toolkit's modes (free play aside) know where this game
  keeps its stages, players or camera.

## Next (see `docs/TOOLKIT.md`, `beat/beat.h`, and `games/` of the original cps2recomp for what a beat-'em-up port says)

1. Find the work-RAM addresses of the players, lives, score and credits with `tools/ramfind.py` / `tracechanges.py`.
2. Name the game's places in `src/` (stage start, camera, character animations) so attract mode, score attack, stage and
   model viewers work.
3. Check the sound driver against the Z80 (`make GAME=ddsom check-sound`) and translated code against the interpreter
   (`make GAME=ddsom check`).
