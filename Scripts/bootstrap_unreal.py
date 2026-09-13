"""Build Riftbound's original content in Unreal Editor 5.8.

Run with UnrealEditor-Cmd.exe <project> -unattended -ExecutePythonScript=<this file>.
For source-audio generation/verification without Unreal: python <this file> --audio-only

Content is generated from code; no downloaded or third-party media is required.
Re-running refreshes the two materials and sound waves but preserves an existing map.
"""

from array import array
import json
import math
from pathlib import Path
import random
import sys
import traceback
import wave


ROOT = Path(__file__).resolve().parents[1]
AUDIO_DIR = ROOT / "SourceArt" / "Audio"
RATE = 44100
TAU = math.tau


def write_wave(name, channels, peak):
    """Normalize original floating-point synthesis to safe 16-bit PCM."""
    count = len(channels[0])
    if not count or any(len(ch) != count for ch in channels):
        raise ValueError("Invalid audio channels for " + name)
    maximum = max(abs(x) for ch in channels for x in ch)
    scale = peak / max(maximum, 0.00001)
    pcm = array("h")
    for i in range(count):
        for ch in channels:
            pcm.append(int(max(-1.0, min(1.0, ch[i] * scale)) * 32767))
    if sys.byteorder != "little":
        pcm.byteswap()
    path = AUDIO_DIR / (name + ".wav")
    with wave.open(str(path), "wb") as out:
        out.setnchannels(len(channels))
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(pcm.tobytes())
    return {"name": name, "path": path.relative_to(ROOT).as_posix(), "seconds": count / RATE,
            "sample_rate": RATE, "channels": len(channels), "peak": peak}


def synthesize_audio():
    AUDIO_DIR.mkdir(parents=True, exist_ok=True)
    rng = random.Random(27091996)
    gun = []
    noise_lp = 0.0
    for i in range(int(RATE * 0.42)):
        t = i / RATE
        noise = rng.uniform(-1, 1)
        noise_lp += 0.13 * (noise - noise_lp)
        attack = min(1.0, t / 0.001)
        click = 0.78 * noise * math.exp(-85 * t)
        body = 0.62 * math.sin(TAU * (95 * t + 24 * (1 - math.exp(-45 * t)))) * math.exp(-26 * t)
        energy = 0.20 * math.sin(TAU * (690 * t + 55 * (1 - math.exp(-15 * t)))) * math.exp(-17 * t)
        tail = 0.32 * noise_lp * math.exp(-14 * t)
        gun.append(attack * (click + body + energy + tail) * min(1.0, (0.42 - t) / 0.012))

    explosion = []
    low = 0.0
    for i in range(int(RATE * 1.8)):
        t = i / RATE
        noise = rng.uniform(-1, 1)
        low += 0.038 * (noise - low)
        crack = 0.30 * noise * math.exp(-27 * t)
        rumble = 1.7 * low * math.exp(-3.4 * t)
        thump = 0.49 * math.sin(TAU * (36 * t + 5 * (1 - math.exp(-11 * t)))) * math.exp(-6.5 * t)
        grit = 0.20 * noise * (0.5 + 0.5 * math.sin(TAU * 24 * t)) * math.exp(-7 * t)
        explosion.append(min(1.0, t / 0.0015) * (crack + rumble + thump + grit) * min(1.0, (1.8 - t) / 0.07))

    hit = []
    for i in range(int(RATE * 0.24)):
        t = i / RATE
        impact = 0.44 * rng.uniform(-1, 1) * math.exp(-70 * t)
        shard = 0.35 * math.sin(TAU * 1260 * t) * math.exp(-30 * t)
        shard += 0.25 * math.sin(TAU * 1831 * t) * math.exp(-40 * t)
        body = 0.55 * math.sin(TAU * (115 * t + 3 * (1 - math.exp(-40 * t)))) * math.exp(-28 * t)
        hit.append(min(1.0, t / 0.0008) * (impact + shard + body) * min(1.0, (0.24 - t) / 0.015))

    # A quiet seamless stereo soundscape. Every oscillator makes an integer
    # number of cycles in the loop; the filtered noise is crossfaded at the seam.
    duration = 16.0
    length = int(duration * RATE)
    fade = int(1.2 * RATE)
    ambience = []
    for channel in range(2):
        samples = []
        low = mid = 0.0
        noise_samples = []
        for i in range(length + fade):
            n = rng.uniform(-1, 1)
            low += 0.009 * (n - low)
            mid += 0.06 * (n - mid)
            noise_samples.append(0.7 * low + 0.20 * mid)
        for i in range(length):
            t = i / RATE
            wind = noise_samples[i]
            if i < fade:
                a = i / fade
                blend = a * a * (3 - 2 * a)
                wind = noise_samples[length + i] * (1 - blend) + wind * blend
            swell = 0.8 + 0.20 * math.sin(TAU * 3 * t / duration + channel * 0.8)
            drone = 0.020 * math.sin(TAU * 48 * t + channel * 0.6)
            drone += 0.014 * math.sin(TAU * 72 * t + 0.40 * math.sin(TAU * t / duration))
            distant = 0.013 * math.sin(TAU * 256 * t + channel * 0.5) * (0.5 + 0.5 * math.sin(TAU * 2 * t / duration)) ** 8
            samples.append(1.9 * wind * swell + drone + distant)
        ambience.append(samples)

    manifest = [write_wave("S_Gun", [gun], 0.75),
                write_wave("S_Explosion", [explosion], 0.8),
                write_wave("S_Hit", [hit], 0.6),
                write_wave("S_Ambient", ambience, 0.17)]
    (AUDIO_DIR / "manifest.json").write_text(json.dumps({
        "provenance": "Original procedural synthesis created for Riftbound. No external samples.",
        "sounds": manifest
    }, indent=2), encoding="utf-8")
    return manifest


def build_unreal_assets(manifest):
    import unreal

    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    editing = unreal.MaterialEditingLibrary

    def log(message):
        unreal.log("RIFTBOUND BOOTSTRAP: " + message)

    def expression(material, cls, x, y, **properties):
        value = editing.create_material_expression(material, cls, x, y)
        if value is None:
            raise RuntimeError("Could not create material expression " + str(cls))
        for name, data in properties.items():
            value.set_editor_property(name, data)
        return value

    def link(source, dest, input_name):
        if not editing.connect_material_expressions(source, "", dest, input_name):
            raise RuntimeError("Could not connect " + source.get_name() + " to " + dest.get_name() + "." + input_name)

    def output(source, prop):
        if not editing.connect_material_property(source, "", prop):
            raise RuntimeError("Could not connect material output " + str(prop))

    def material(name):
        path = "/Game/Materials/" + name
        value = assets.load_asset(path) if assets.does_asset_exist(path) else None
        if value is None:
            value = asset_tools.create_asset(name, "/Game/Materials", unreal.Material, unreal.MaterialFactoryNew())
        if value is None:
            raise RuntimeError("Could not create " + path)
        editing.delete_all_material_expressions(value)
        editing.set_base_material_usage(value, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
        return value

    def save_material(value):
        editing.layout_material_expressions(value)
        errors = editing.recompile_material(value)
        if errors:
            raise RuntimeError("Material shader compilation failed for " + value.get_name() + ": " + "; ".join(errors))
        if not assets.save_loaded_asset(value, False):
            raise RuntimeError("Could not save " + value.get_path_name())
        log("Saved " + value.get_path_name())

    log("Creating original world-space surface material")
    surface = material("M_Surface")
    tint = expression(surface, unreal.MaterialExpressionVectorParameter, -650, 0,
                      parameter_name="Tint", default_value=unreal.LinearColor(0.14, 0.30, 0.20, 1),
                      desc="Runtime tint for foliage, bark, stone, terrain and equipment")
    world = expression(surface, unreal.MaterialExpressionWorldPosition, -1000, 280)
    noise = expression(surface, unreal.MaterialExpressionNoise, -750, 270,
                       noise_function=unreal.NoiseFunction.NOISEFUNCTION_GRADIENT_TEX3D,
                       scale=0.016, levels=2, quality=1, level_scale=2.6,
                       output_min=0.65, output_max=1.25, turbulence=False,
                       desc="Two inexpensive world-space octaves prevent stretched UVs on procedural geometry")
    # UE 5.8 labels this pin according to its coordinate origin. Empty selects
    # the first input, avoiding a dependency on that display label.
    link(world, noise, "")
    color = expression(surface, unreal.MaterialExpressionMultiply, -330, 0)
    link(tint, color, "A")
    link(noise, color, "B")
    output(color, unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = expression(surface, unreal.MaterialExpressionConstant, -320, 250, r=0.83)
    output(roughness, unreal.MaterialProperty.MP_ROUGHNESS)
    specular = expression(surface, unreal.MaterialExpressionConstant, -320, 380, r=0.24)
    output(specular, unreal.MaterialProperty.MP_SPECULAR)
    save_material(surface)

    log("Creating rift glow material")
    glow = material("M_Glow")
    glow_tint = expression(glow, unreal.MaterialExpressionVectorParameter, -640, 0,
                           parameter_name="Tint", default_value=unreal.LinearColor(0.08, 0.95, 0.66, 1))
    strength = expression(glow, unreal.MaterialExpressionScalarParameter, -640, 220,
                          parameter_name="Glow", default_value=4.0)
    emission = expression(glow, unreal.MaterialExpressionMultiply, -320, 0)
    link(glow_tint, emission, "A")
    link(strength, emission, "B")
    output(emission, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    dark = expression(glow, unreal.MaterialExpressionMultiply, -300, 280, const_b=0.015)
    link(glow_tint, dark, "A")
    output(dark, unreal.MaterialProperty.MP_BASE_COLOR)
    glow_roughness = expression(glow, unreal.MaterialExpressionConstant, -300, 440, r=0.55)
    output(glow_roughness, unreal.MaterialProperty.MP_ROUGHNESS)
    save_material(glow)

    log("Importing original sounds")
    tasks = []
    for source in manifest:
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", str(ROOT / source["path"]))
        task.set_editor_property("destination_path", "/Game/Audio")
        task.set_editor_property("destination_name", source["name"])
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        task.set_editor_property("factory", unreal.SoundFactory())
        tasks.append(task)
    asset_tools.import_asset_tasks(tasks)
    for source in manifest:
        path = "/Game/Audio/" + source["name"]
        sound = assets.load_asset(path)
        if sound is None or not isinstance(sound, unreal.SoundWave):
            raise RuntimeError("Sound import did not produce expected SoundWave " + path)
        sound.set_editor_property("looping", source["name"] == "S_Ambient")
        sound.set_editor_property("compression_quality", 80)
        # Ambient is pre-mixed quietly; runtime can lower its component volume.
        sound.set_editor_property("volume", 1.0)
        if not assets.save_loaded_asset(sound, False):
            raise RuntimeError("Could not save " + path)
        log("Saved " + path)

    path = "/Game/Maps/Forest"
    log("Preparing blank runtime-generated forest map")
    if assets.does_asset_exist(path):
        if not levels.load_level(path):
            raise RuntimeError("Could not load existing map " + path)
    elif not levels.new_level(path, False):
        raise RuntimeError("Could not create blank map " + path)
    if not levels.save_current_level():
        raise RuntimeError("Could not save map " + path)

    required = ["/Game/Materials/M_Surface", "/Game/Materials/M_Glow", path]
    required.extend("/Game/Audio/" + x["name"] for x in manifest)
    missing = [name for name in required if not assets.does_asset_exist(name)]
    if missing:
        raise RuntimeError("Missing assets after bootstrap: " + ", ".join(missing))
    report = {"status": "success", "assets": required, "audio": manifest}
    report_dir = ROOT / "Saved"
    report_dir.mkdir(parents=True, exist_ok=True)
    (report_dir / "BootstrapReport.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    log("SUCCESS: all 7 assets saved and verified")


def main():
    manifest = synthesize_audio()
    if "--audio-only" in sys.argv:
        print(json.dumps(manifest, indent=2))
        return
    try:
        build_unreal_assets(manifest)
    except Exception:
        import unreal
        unreal.log_error("RIFTBOUND BOOTSTRAP FAILED:\n" + traceback.format_exc())
        raise


if __name__ == "__main__":
    main()
