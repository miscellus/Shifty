# Shifty Co

![Shifty Co title screen](./readme/splash.png)

A Sokoban-style game written in 8085 assembly language for the NEC PC-8201A (and Tandy TRS-80 Model 100, Olivetti M10, and the Kyotronic KC-85).

## The Game
Shifty Co is a classic Sokoban-style game. The goal is for the main character to reach an open door, finishing the level and progressing to the next.

Easier said than done. The path to the door may be blocked by a hole, for instance. In that case, stones are scattered about that you can push. Push a stone into a hole to fill it and make it traversable.

But the door could be locked. In that case, look for shooting targets. The doors in the level magically decide to unlock and open if you destroy all such shooting targets (what a weird place.)

Sometimes the path to the door goes through a narrow passage with turns and bends that you need to push a stone (or something else) through. For that, we have the arrow tiles.

Arrow tiles can be pushed like the stones, but something special happens when they are pushed all the way up against a wall. If you try to push into an arrow that is pointing sideways compared to your direction of movement, you teleport to the front of where the arrow is pointing. Very exciting! And what happens if you push a stone into the perpendicular arrow? It teleports too! What about pushing an arrow into an arrow, surely not? YES! Can I push a whole train of stones, arrows, and shooting targets into an arrow? I think you know the answer.

If you screw up (you noob), you can hit Enter to reset the level back to how it started. Too harsh for you? Hit Backspace or Z to undo a single move. You have an undocumented number of moves that can be undone like that.

Good luck entering the eerie corporate catacombs - *cough* - I mean offices of Shifty Co.

![Example of a Shifty Co Level](./readme/level-example.png)

## Thanks
I want to thank [Eydi av Hamri](https://www.youtube.com/c/EydiavHamriChannel) for helping me with the visuals of the game, and generally always being a pleasure to hang out and make games with.

Thank you [Asbjørn Tølbøl Brask](https://www.atbrask.dk/) for **porting this game to Commodore 64!** And for consistently nerd-sniping me into crazier and crazier things while listening to me ramble about this game during our lunch breaks at work.

Also, thank you to my brother [Jesper Kjær-Galle](https://mangestreger.dk) for being the first (I believe) to finish the entire game on the real hardware and giving me important feedback.

Of course thanks to [Marinus Oosters](https://github.com/marinuso/asm8085) for his 8085 assembler that I have really enjoyed using and adding features to, as well as the authors of the VirtualT emulator.

## How and Why Shifty Co Exists
I was habitually scrolling Den Blå Avis, a Danish platform for buying and selling used items, where I lucked into buying an NEC PC-8201A from 1983 for just 250 kr. (US $39). I was stoked. I love old computers, and this was now the oldest in my collection, by a good margin. I also love games, so after playing around with the built-in BASIC interpreter on the 8201 for a couple of evenings, I decided to try and download some games for it and transfer them via serial RS-232 using its built-in terminal software TELCOM.

I quickly realized that I wasn't going to find tons of games for the 8201. When the machine was new, I believe it was mostly used as a portable word processor, for instance by journalists who wrote reports on it in the field.

There were some games that I could find, e.g., [this golf game written in BASIC](https://www.web8201.net/Files/LIBRARY_web8201/Games/GOLF.BA). Actually, the only games I could find for it were written in BASIC.

So I impulsively decided that I was going to write a machine language game for it, combining my fondness for both games and assembly programming.

As you might imagine, this took me on a bit of a journey. I have previously written (part of) [a game for the Nintendo Game Boy](https://www.miscellus.com/projects/gameboy-game/). But the homebrew scene for Game Boy games is a lot more established / existing than for the NEC PC-8201A. I had to build tools to aid development.

### Hurdle No. 1
The built-in terminal software on the 8201, TELCOM, can only receive printable ASCII, so no machine code transfer! I therefore wrote (asked an LLM to write) a Python script that would convert the machine code to a BASIC program that would poke the raw machine code into memory at the correct place and jump to it. It worked, but man it was slow to iterate like that:

```BASIC
10 Work on the game code;
20 assemble the game;
30 run the binary through the Python script;
40 open a serial terminal on my modern PC;
50 set the baud rate, parity, stop bits, and flow-control;
60 then open the TELCOM program on the 8201;
70 then set its baud rate, parity, stop bits, and flow-control;
80 finally tranfer;
90 exit TELCOM;
100 enter BASIC;
110 load the game loader BASIC script;
120 crash the 8201 because my game code was buggy;
130 goto 10.
```

So I figured I would mostly test on the venerable [VirtualT emulator](https://sourceforge.net/projects/virtualt/) that emulates the entire KC-85 family of computers, including the NEC PC-8201A.

### Hurdle No. 2
VirtualT allowed for much faster code iteration. I could do just lines `10` and `20` from above, and then drag and drop the output onto the VirtualT emulator. That would produce the GAME.CO file that I could then run. But this was still too slow for my liking as I would have to completely reset the emulator every time (either a bug in VirtualT or me just being dumb).

So I decided to side-quest and write my own NEC PC-8201A emulator. Since the emulator was not the main thing I was trying to achieve, I did use an LLM to speed up some of the busywork of implementing every single 8085 instruction in the emulator. But I would not call it vibe coding, because I had a lot of opinions about the output and also a lot of manual work on the emulator, I mostly used LLMs to quickly get off the ground and avoid the blank-canvas effect.

The writing of the emulator coincided with my friend Asbjørn nerd-sniping me into porting the game to the web so it could be played in the browser and even on a smartphone with touch controls... Thanks Asbjørn!
Because of this, I decided to write the emulator as a standalone C99 library that could be compiled directly to WebAssembly. Because the emulator doesn't depend on anything (not even the C standard library), I could ditch Emscripten and just use Clang.

I again turned to an LLM to make the HTML/CSS/JavaScript frontend that would hook up to the WASM emulator core. Web stuff generally doesn't appeal to me, so I was okay outsourcing most of this part to the LLM. I was actually impressed with the LCD screen shader. Sorry to whoever wrote the original thing on [ShaderToy](https://www.shadertoy.com/) that the LLM likely trained on, and that I can therefore not give credit to directly...

### Hurdle No. 3
As the game grew in complexity, it got harder to debug. I was just reading the code carefully, and prompting myself not to make any mistakes! But alas, bugs crept in, especially when working on the game's push mechanic.

So the next side-quest was adding a debugger to the emulator. It was actually surprisingly easy to do. It required a little more bookkeeping in the emulator (keeping track of the call stack), but besides that, the main thing was augmenting the assembler with a feature to output debug information in a web-friendly format (JSON). The assembler I use is the very awesome [**asm8085** by Marinus Oosters](https://github.com/marinuso/asm8085), which was written with the KC-85 family of computers in mind. I have already made a few tweaks, which were super easy thanks to the well-written code by Marinus!

Now I have a source level debugger with creature comforts such as:
- Breakpoints
- Step into
- Step over
- Step out

![Screenshot of debugger](./readme/debugger.png)
