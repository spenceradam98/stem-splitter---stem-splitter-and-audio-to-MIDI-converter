stem-scribe - stem splitter and audio-to-MIDI

This installs:
  - stem-scribe.vst3 into C:\Program Files\Common Files\VST3 (the standard
    folder every host scans)
  - The stem-scribe standalone app (optional): drop a song on it, then drag
    stems and MIDI out
  - The HTDemucs separation model (about 80 MB) into
    C:\ProgramData\YourBrand\stem-scribe, shared by the plugin and the app

If Windows shows "Windows protected your PC", click "More info" and then
"Run anyway". This appears because this test build is not code-signed yet.

After installing, open your DAW and rescan plugins if stem-scribe doesn't
show up. In FL Studio: Options > Manage plugins > Find more plugins.

Splitting a 4-minute song takes a few minutes on a recent PC; results are
cached, so reopening a session is quick.
