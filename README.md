# Reactive PNG for OBS

An OBS source that shows a PNG, lights it up while you talk, and can give it a small bounce.

## Install

Requires 64-bit Windows and OBS 32. Built and tested with OBS 32.1.2.

1. Close OBS.
2. Download `reactive-png-windows-x64.zip` from the [Releases](../../releases) page.
3. Extract it into `C:\ProgramData\obs-studio\plugins`, creating the `plugins` folder if it doesn't exist. Windows may ask for administrator permission.
4. Check that the DLL ended up here:

   ```text
   C:\ProgramData\obs-studio\plugins\reactive-png\bin\64bit\reactive-png.dll
   ```

5. Start OBS. **Reactive PNG** now appears in the list of sources.

To uninstall, close OBS and delete the `reactive-png` folder.

## Build from source

Windows x64, with OBS installed. Close OBS, then from a PowerShell window in this folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Install
```

The first run downloads a portable compiler (about 180 MB) and the OBS headers into `.deps\`. Nothing else is installed on your system. The plugin is copied to `%ProgramData%\obs-studio\plugins\reactive-png`. If that copy is denied, run the command from an administrator PowerShell.

Leaving off `-Install` builds into `dist\` without installing. After an OBS update, run the script again so the plugin matches the new version.

## Use

1. In a scene, add a source: **Reactive PNG**.
2. Pick your image and your microphone.
3. Talk, and watch the Audio Mixer meter. Set **Talking threshold** just above the level your room noise reaches.

| Setting | Default | What it does |
| --- | --- | --- |
| Talking threshold | -40 dB | How loud counts as talking. Same scale as the OBS mixer. |
| Stay lit after talking | 200 ms | Bridges short gaps between words. A gap longer than this counts as a new start, so the next word hops again. |
| Idle brightness | 50% | Brightness when silent. |
| Talking brightness | 100% | Brightness when talking. Above 100% brightens the image. |
| Fade time | 80 ms | How quickly it moves between the two. |
| Bounce when talking | On | Turns the bounce on or off. |
| Bounce height | 5% | Hop height, as a percentage of the image height. |
| Bounce duration | 250 ms | Length of the hop. It hops once each time you start talking. |

## Good to know

- It reacts to the mic after that mic's filters, so a noise gate or noise suppression also cuts false triggers. Muting the mic, including through push-to-talk, stops the reaction.
- The source is taller than the image by the bounce height, to leave room for the hop. Turning the bounce on or off therefore moves the image slightly in the scene.
- If you overwrite the PNG file at the same path, the change appears after restarting OBS or choosing a different file.

## Performance

- Each frame is one sprite draw using a shader built into OBS, with no render targets or allocations.
- The audio callback runs about 47 times a second. It does one pass over the samples and updates an atomic counter, with no locks, logs or allocations.
- While the source is not in a visible scene, it disconnects from the mic entirely.
- The PNG is decoded only when the file path changes, not when a slider moves. Picking a new image decodes it on the graphics thread, so a very large file can cause a brief hitch at that moment.

## License

Licensed under the GNU General Public License, version 2 or later. See [LICENSE](LICENSE).
