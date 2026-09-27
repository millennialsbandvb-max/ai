# VST2 plugins on the MPC Live II

This isn't new firmware. MPC OS already contains a hidden JUCE plugin host that can load Linux VST2 plugins
(`.so` files built for 32-bit ARM). The open-source project `sd88me/mpc-vst-plugins` (uploaded here as
`mpc-vst-plugins-main.zip`) builds plugins for that host. You don't reflash anything. You need to:

1. copy a plugin `.so` to the device,
2. add one line for it to `MPC.settings`,
3. restart MPC.

It was verified on an Akai Force (Gen1, armv7). The Live II is also Gen1 and runs the same `MPC` program, so it
should behave the same. Gen2 units (Live III) are reported to be locked down.

## What you need

- **A root shell on the Live II (SSH).** Stock MPC OS doesn't give you one. The upstream project got it with a
  modded OS (MockbaMod). Without shell access you can't edit `MPC.settings`, and nothing else works.
- A computer on the same network.

## Step 1: check the device (read-only)

```sh
ssh root@<live2-ip> sh -s < tools/probe_device.sh     # from the mpc-vst-plugins folder
```

You want `armv7l`, `VST2 (juce::VSTPluginFormat): yes`, a `pluginList` / `-arm` settings key, and a path like
`/media/az01-internal/Settings/MPC/MPC.settings`.

## Step 2: install a known-working plugin first

Get the hardware-tested DX7 release by running the **Fetch Known Working MPC DX7 Release** workflow in this repo's
Actions tab (or download it from `github.com/sd88me/mpc-vst-dx7/releases`). Then:

```sh
scp DX7-Dexed-0.4.0-mpc-armv7.zip root@<live2-ip>:/tmp/
ssh root@<live2-ip>
cd /tmp && unzip DX7-Dexed-0.4.0-mpc-armv7.zip && cd DX7*/   # folder name may differ
sh install.sh            # stops MPC, backs up MPC.settings, adds the entry, restarts MPC
```

On the MPC, add a plugin track and pick **DX7** from the plugin browser. If it shows up and plays, the route works
on your unit.

## Step 3: install by hand (any `.so`, e.g. the gain PoC built in this repo)

```sh
systemctl stop acvs                                   # stops MPC; save your project first
cp /media/az01-internal/Settings/MPC/MPC.settings /media/az01-internal/Settings/MPC/MPC.settings.bak
mkdir -p /sdcard/vst && cp MPC-Gain-PoC.so /sdcard/vst/
vi /media/az01-internal/Settings/MPC/MPC.settings
```

Inside `<VALUE name="pluginList-arm"><KNOWNPLUGINS> … </KNOWNPLUGINS></VALUE>` add (create that block just before
`</PROPERTIES>` if it doesn't exist):

```xml
<PLUGIN name="MPC Gain" descriptiveName="MPC Gain" format="VST" category="Effect" manufacturer="PoC"
        version="1.0" file="/sdcard/vst/MPC-Gain-PoC.so" uid="<the plugin's uniqueID in hex>" isInstrument="0"
        fileTime="0" infoUpdateTime="0" numInputs="2" numOutputs="2" isShell="0"/>
```

Then `systemctl start acvs`. If the XML is malformed, MPC resets the settings to defaults, so keep the backup.

## Can I load any VST2 plugin from my computer?

No. Windows `.dll` and macOS `.vst` files won't load. A plugin has to be compiled for 32-bit ARM Linux against
glibc 2.36 or older (`arm32v7/gcc:12`), and it has to fit the CPU budget (about 2.9 ms per 128-sample block,
shared with the whole project). Open-source plugins can be ported. See `docs/PORTING.md` in `mpc-vst-plugins`.

## If it doesn't show up

- Plugin missing from the browser: `MPC.settings` wasn't edited while MPC was stopped, the path in `file=` is
  wrong, or the XML was reset (check for your entry after the restart).
- MPC crashes on insert: run `src/mpc_vst_dlopen_probe.c` (the **Build MPC VST dlopen diagnostic probe**
  workflow) on the device: `./probe /sdcard/vst/x.so /sdcard/vst/x.log`, then read the log. `dlopen FAIL`
  usually means a glibc or dependency mismatch.
- The AEffect magic has to be `0x56737450` (`'VstP'`). The snippet from the forum post has it wrong.

Editing `MPC.settings` and modding the OS are at your own risk. Back up first.
