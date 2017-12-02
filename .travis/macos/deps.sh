#!/bin/sh -ex

rvm get head --auto-dotfiles
brew update
brew upgrade
brew install qt5 sdl2 dylibbundler p7zip
