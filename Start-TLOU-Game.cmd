@echo off
rem The Last of Us Remastered - starts the game with the settings the launcher saved, without
rem opening the launcher (tools/run_game.py reads analysis\tlou-user\launcher.json). Anything given
rem here is passed on, e.g.: Start-TLOU-Game.cmd --seconds 60
cd /d "%~dp0"
python tools\run_game.py %*
