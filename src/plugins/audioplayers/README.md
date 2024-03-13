## audioplayers plugin

### Requirements

- `audioplayers` version `^4.0.0`
- Working gstreamer installation, including corresponding audio plugin (e.g. `gstreamer1.0-alsa`)

### Troubleshooting

- Check that you can list ALSA devices via command `aplay -L`;
- Check that you can launch `playbin` on any audio file via `gst-launch`;
- Make sure `pulseaudio` is deleted

### pulseaudio

Please note that plugin was not tested with `pulseaudio` and it is up to you to make gstreamer work via it.
As `pulseaudio` takes full control over audio devices, `ALSA` will no longer function correctly with `pulseaudio` installed
