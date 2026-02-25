# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

BSc thesis at DTU (Technical University of Denmark) for designing the first version of a PCB for a robotic hand used in post-stroke hand rehabilitation. The robotic hand helps patients regain motor function through guided exercises.

This PCB replaces the current prototype's Arduino dev-boards and loose wiring. It is a rectangular board with no physical dimension constraints (the matching mechanical enclosure is being developed in parallel). The goal is to validate the electrical wiring, layout, and component choices.

### Key PCB Subsystems

- **Microcontroller** with wireless connectivity (Bluetooth/Wi-Fi) to a smart device for receiving commands and exercise data
- **Local storage** for long-term logging of hand position/movement history and hand-tracking data from the smart device (medical data — must stay local, no cloud)
- **User nudge system** — audio and/or visual stimuli to remind patients of exercises
- **Motor driver** — controls 5 servo motors (one per finger)
- **Battery power** — target ~1 week autonomy (adjustable if infeasible)

## Repository Structure
You should keep this section updated as the repo evolves, so that Claude Code can understand the overall structure and where to find key files.
The root repo contains two **Overleaf git submodules** and reference PDFs:

- `plan/` — Project Definition Report (PDR), due early in the project. Overleaf submodule.
- `thesis/` — Final BSc thesis. Overleaf submodule. Currently just the DTU LaTeX template.
- `BSc Project Proposal.pdf` — Original project proposal with background and scope.
- `Instructions_from_an_Active_Robotic_Hand_Increase_Body_Ownership_and_Task_Clarity.pdf` — Published paper on the robotic hand project as a whole.

## LaTeX

Both `plan/` and `thesis/` are LaTeX projects compiled with **XeLaTeX**. But the compilation process is mostly abstracted away by Overleaf, so you can focus on editing the `.tex` files without worrying about the build system.

### Plan (PDR)
The main file is `plan/Thesis.tex`. Key content chapters to edit:
- `chapters/ProjectDescription.tex` — Background, prior work, research question, goals, methods, ethics
- `chapters/LearningObjectives.tex` — Intended learning objectives
- `chapters/Plan.tex` — Gantt chart, activities, milestones, deliverables, risk analysis

Static metadata (author, title, subtitle, paper size) is configured in `plan/preamble/static.tex`.

### Thesis

The main file is `thesis/main.tex`. Static metadata is in `thesis/Setup/Statics.tex`. Content chapters go in `thesis/Chapters/`. Currently contains only template examples — no thesis content yet.

## Important Conventions

- The `plan/` and `thesis/` folders are **separate Overleaf git repos** linked as submodules. Commits and pushes inside them sync to Overleaf. Work within each submodule directory when making changes.
- The plan template uses `\note{...}` commands throughout — these are **instructional placeholders** explaining what each section should contain. Replace them with actual content.
- The plan's `static.tex` currently defaults to MSc. — it needs to be changed to BSc.
- Bibliography entries go in `plan/bibliography/Bibliography.bib` and `thesis/bibliography.bib`.
