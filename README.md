# cargame — Terminal Car Game 🚗💨

A lightweight cross-platform terminal racing game written in C++ and packaged as a Python CLI wrapper.
Players can install via pip and run a native binary using the `car-game` command.

---

## Quick start

# on Debian/Ubuntu
sudo apt update && sudo apt install -y pipx
pipx ensurepath
pipx install cargame

# then run
car-game

if you get any issue try this commands

# 1) Temporarily put ~/.local/bin at front of PATH for this session
export PATH="$HOME/.local/bin:$PATH"

# 2) Verify the binary exists and is executable
ls -l "$HOME/.local/bin/car-game" || true

# 3) Verify shell can now find it and run it (or show its path)
command -v car-game || which car-game || true

# 4) Try to run it
car-game || true

# or run 
car-game

## For windows
pip install cargame

# run
car-game