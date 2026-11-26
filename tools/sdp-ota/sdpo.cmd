@echo off
rem SDP firmware tool (see "sdpo --help"). SDP_PYTHON picks the interpreter: Python 3.11+ with pyserial.
rem Without it: the py launcher, then python on PATH.
rem The exit code is the tool's (upload: 0 verified, 1 failed, 2 rolled back, 3 unknown). Inside ( ) blocks
rem a bare "exit /b" returned 0, so the code is read with delayed expansion.
setlocal EnableDelayedExpansion
if defined SDP_PYTHON (
    "%SDP_PYTHON%" -I -B "%~dp0sdp_ota.py" %*
    exit /b !errorlevel!
)
where py >nul 2>nul && (
    py -3 -I -B "%~dp0sdp_ota.py" %*
    exit /b !errorlevel!
)
python -I -B "%~dp0sdp_ota.py" %*
exit /b !errorlevel!
