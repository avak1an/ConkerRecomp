<div align="center">

<img src="assets/conker-live-recomped-logo-v2-c.png" alt="Conker: Live & Recomped" width="760">

# Conker: Live & Recomped

### A native Windows recompilation project for *Conker: Live & Reloaded*

[![Platform](https://img.shields.io/badge/platform-Windows-0078D4?style=for-the-badge&logo=windows11&logoColor=white)](#platform)
[![Status](https://img.shields.io/badge/status-Work%20in%20Progress-orange?style=for-the-badge)](#project-status)
[![Game Files](https://img.shields.io/badge/original%20game-required-8A2BE2?style=for-the-badge)](#game-files)
[![Project](https://img.shields.io/badge/project-unofficial-blue?style=for-the-badge)](#legal)

**Repository:** `ConkerRecomp`

</div>

---

## 🐿️ About

**Conker: Live & Recomped** is an unofficial native recompilation project for the original Xbox release of **Conker: Live & Reloaded**.

The goal is to run the game as a native Windows application while preserving the behavior of the original Xbox game as closely as practical.

This project is **not an emulator** and does **not** distribute the original game, its assets, or extracted game data.

---

## 🎯 Project Goals

- Run **Conker: Live & Reloaded** natively on modern Windows.
- Preserve the original game's logic and behavior through static/native recompilation.
- Replace Xbox-specific platform services with clean Windows-compatible implementations.
- Translate original Xbox graphics behavior to a modern PC rendering backend.
- Support modern PC input, including **keyboard and mouse**.
- Require users to provide their **own original game files**.
- Make the project reproducible, understandable, and maintainable as an open-source effort.

---

## 💿 Game Files

> [!IMPORTANT]
> **No original Conker game files are included in this repository or in planned releases.**

Users will need to provide their own legal copy of **Conker: Live & Reloaded**.

---

## 🚧 Project Status

> [!WARNING]
> **Very early development — not currently ready for normal gameplay.**

The project has progressed well beyond initial XBE startup and is now reconstructing the original game's live Frontend/title runtime.

### Current milestones

- ✅ Original Xbox executable analysis and native recompilation pipeline
- ✅ Xbox memory / register compatibility layer
- ✅ Kernel and file-system bridge work
- ✅ Frontend package loading and CAFF parsing
- ✅ Frontend `.data` and `.gpu` section handling
- ✅ Resource relocation and package graph reconstruction
- ✅ Frontend database loading / publication
- ✅ Menu entity construction
- ✅ Tavern actor-manager discovery and scene traversal
- ✅ Native geometry traversal and cache compilation
- ✅ Stable recurring frame/update loop
- 🚧 Scene / title-manager integration
- 🚧 Rendering the complete Frontend scene
- 🚧 GPU translation / rendering correctness
- 🚧 Audio
- 🚧 Gameplay
- 🚧 Keyboard / mouse support
- 🚧 User-friendly game importer / installer

The exact list changes frequently while low-level Xbox behavior is recovered.

---

## 🖥️ Platform

### Primary target

**Windows x64**

Development currently focuses on Windows first. Other platforms may be considered later, but portability is not currently the priority.

---

## 📁 Planned User Workflow

Eventually, a user should be able to:

```text
1. Download ConkerRecomp
2. Launch the setup/import tool
3. Select their own Conker: Live & Reloaded disc / ISO
4. Let the tool verify and extract the required data
5. Launch Conker: Live & Recomped
```

---

## 🚀 Future Goals

Once the recomp reaches a stable, accurate, and playable release, the project can begin expanding beyond basic compatibility.

### Multiplayer

- Restore and support the original multiplayer functionality.
- Investigate modern networking options where appropriate.
- Preserve the original multiplayer gameplay while making it practical to use on modern PCs.

### Mod Support

A stable native PC version creates opportunities for a proper modding ecosystem, including:

- custom content and gameplay modifications;
- easier resource replacement;
- new gameplay features;
- community-made fixes and improvements;
- scripting/tooling where technically practical.

### Enhanced PC Features

Potential post-stability improvements include:

- **HD / higher-resolution rendering**
- **widescreen and ultrawide support**
- higher frame-rate support where game logic allows it
- improved graphics options
- modern controller support
- keyboard and mouse support
- quality-of-life improvements
- optional new features that remain separate from the original-game compatibility path

The first priority remains achieving a stable and accurate recompilation. Enhancements should come **after** the original game is running correctly.

## ⚖️ Legal

**Conker: Live & Recomped / ConkerRecomp is an unofficial fan-made project.**

It is not affiliated with, sponsored by, approved by, or endorsed by **Microsoft**, **Rare**, or any other rights holder associated with *Conker: Live & Reloaded*.

*Conker*, *Conker: Live & Reloaded*, Xbox, and related names, characters, artwork, and game content are the property of their respective owners.

This repository does **not** provide the original game and is intended to require users to supply their own copy of the game.

The project logo and other original repository artwork are intended only to identify this unofficial fan project.

---

<div align="center">

### 🐿️ Conker: Live & Recomped

**Native recompilation. Original game required. No game data included.**

<sub>Built with a lot of disassembly, debugging, caffeine, and questionable squirrel-related decisions.</sub>

</div>

---

## 🙌 Special Thanks

This project would not be possible without the work of the following open-source projects and their contributors.

### [XboxRecomp](https://github.com/sp00nznet/xboxrecomp)

**XboxRecomp** provides the core static recompilation foundation used by this project.

It is used to:

- analyze and recompile the original Xbox executable;
- translate original Xbox code into native code that can be rebuilt for Windows;
- provide the base runtime structure used while recovering missing functions and continuations;
- support the ongoing work of reproducing Xbox behavior outside the original hardware.

A huge thank-you to the XboxRecomp developers and contributors for making this kind of project possible.

### [Xemu](https://github.com/xemu-project/xemu)

**Xemu** has been an essential debugging and behavioral reference throughout development.

It is used to:

- run the original game as a reference implementation;
- inspect registers, memory, stack layouts, globals, object structures, and callbacks;
- compare retail control flow against the recompiled build;
- capture original resource-loading, CAFF parsing, scene construction, actor-manager, camera, and rendering behavior;
- verify the expected Xbox-visible state before reproducing that behavior in the Windows recomp.

Xemu is used as a **debugging/reference tool only**. The finished recomp is not intended to require Xemu.

A huge thank-you to the Xemu team and contributors for their work on original Xbox emulation.

### [CLR_Unpack](https://github.com/birdytsc/clr_unpack)

**CLR_Unpack** is used to unpack and inspect *Conker: Live & Reloaded* resource/package data.

It has been especially useful for:

- unpacking `.rbm` package data;
- inspecting package contents and binary layouts;
- validating decoded Frontend resources;
- comparing file-relative structures with the runtime objects observed in Xemu;
- helping identify relocation targets, database objects, UI data, actor data, and other resources required by the recomp.

A huge thank-you to the CLR_Unpack author and contributors for making the game's package format easier to study.

> [!NOTE]
> These projects are independent projects. Their inclusion here is an acknowledgment of the tools used during development and does not imply affiliation with or endorsement of ConkerRecomp.
