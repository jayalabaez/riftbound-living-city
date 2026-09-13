# Generates the LIVING CITY Phase 0 level.
#
# A .umap is a binary asset, so it cannot be written by hand or reviewed in a diff. Rather
# than ask a human to click through the editor, this builds the level from code via the
# Python scripting plugin (already enabled in Riftbound.uproject).
#
# Run headlessly:
#   UnrealEditor-Cmd.exe <project> -run=pythonscript -script="Scripts/bootstrap_livingcity_map.py"
#
# Idempotent: re-running overwrites the level with the same content. Nothing here touches
# any Riftbound asset.

import unreal

LEVEL_PATH = "/Game/LivingCity/Maps/L_LivingCity"
GAME_MODE = "/Script/LivingCityGame.LivingCityGameMode"


def log(msg):
    unreal.log("[LivingCity] " + msg)


def warn(msg):
    unreal.log_warning("[LivingCity] " + msg)


def get_level_subsystem():
    try:
        return unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    except Exception as exc:
        warn("LevelEditorSubsystem unavailable: %s" % exc)
        return None


def get_actor_subsystem():
    try:
        return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    except Exception as exc:
        warn("EditorActorSubsystem unavailable: %s" % exc)
        return None


def spawn(actor_subsystem, cls, location=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0), label=None):
    """Spawn an actor, tolerating API differences between engine versions."""
    loc = unreal.Vector(location[0], location[1], location[2])
    rot = unreal.Rotator(rotation[0], rotation[1], rotation[2])
    actor = None
    if actor_subsystem:
        actor = actor_subsystem.spawn_actor_from_class(cls, loc, rot)
    else:
        actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, loc, rot)
    if actor and label:
        try:
            actor.set_actor_label(label)
        except Exception:
            pass
    return actor


def build():
    level_sub = get_level_subsystem()
    actor_sub = get_actor_subsystem()

    if not level_sub:
        warn("Cannot create the level without LevelEditorSubsystem. Aborting.")
        return False

    # Make sure the folder exists so new_level does not fail on a missing package path.
    try:
        unreal.EditorAssetLibrary.make_directory("/Game/LivingCity/Maps")
    except Exception:
        pass

    log("Creating empty level at %s" % LEVEL_PATH)
    if not level_sub.new_level(LEVEL_PATH):
        warn("new_level failed for %s" % LEVEL_PATH)
        return False

    # ---- lighting ---------------------------------------------------------------------
    # The project sets r.AllowStaticLighting=False, so every light must be movable or it
    # will not contribute at all.
    sun = spawn(actor_sub, unreal.DirectionalLight, (0.0, 0.0, 2000.0), (-46.0, 30.0, 0.0), "Sun")
    if sun:
        try:
            comp = sun.directional_light_component
            comp.set_mobility(unreal.ComponentMobility.MOVABLE)
            comp.set_intensity(6.0)
            comp.set_light_color(unreal.Color(255, 247, 232, 255))
            comp.set_editor_property("atmosphere_sun_light", True)
        except Exception as exc:
            warn("could not configure the directional light: %s" % exc)

    sky = spawn(actor_sub, unreal.SkyLight, (0.0, 0.0, 1500.0), (0.0, 0.0, 0.0), "SkyLight")
    if sky:
        try:
            comp = sky.sky_light_component
            comp.set_mobility(unreal.ComponentMobility.MOVABLE)
            comp.set_intensity(1.0)
            comp.set_editor_property("real_time_capture", True)
        except Exception as exc:
            warn("could not configure the sky light: %s" % exc)

    spawn(actor_sub, unreal.SkyAtmosphere, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "SkyAtmosphere")

    # ---- ground -----------------------------------------------------------------------
    # The engine cube is 100 cm, so scale 100 gives a 100 m plate - comfortably larger than
    # the placeholder's wander box (64 m each way, clamped in the sim).
    floor = spawn(actor_sub, unreal.StaticMeshActor, (0.0, 0.0, -50.0), (0.0, 0.0, 0.0), "Floor")
    if floor:
        try:
            cube = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube.Cube")
            comp = floor.static_mesh_component
            comp.set_mobility(unreal.ComponentMobility.STATIC)
            comp.set_static_mesh(cube)
            floor.set_actor_scale3d(unreal.Vector(140.0, 140.0, 1.0))
            material = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/BasicShapeMaterial")
            if material:
                comp.set_material(0, material)
        except Exception as exc:
            warn("could not configure the floor: %s" % exc)

    # ---- player start -----------------------------------------------------------------
    # Behind and above the origin, angled down at the placeholder so the cube is on screen
    # the moment the level opens.
    spawn(actor_sub, unreal.PlayerStart, (-900.0, 0.0, 450.0), (-18.0, 0.0, 0.0), "PlayerStart")

    # ---- game mode --------------------------------------------------------------------
    # The launcher also passes ?game= on the URL, so this is belt and braces - and it means
    # the level behaves correctly if opened directly in the editor.
    try:
        world = unreal.EditorLevelLibrary.get_editor_world()
        settings = world.get_world_settings()
        gm = unreal.load_object(None, GAME_MODE)
        if gm:
            settings.set_editor_property("default_game_mode", gm)
            log("GameMode override set to %s" % GAME_MODE)
        else:
            warn("could not load %s - is LivingCityGame compiled?" % GAME_MODE)
    except Exception as exc:
        warn("could not set the GameMode override: %s" % exc)

    # ---- save -------------------------------------------------------------------------
    if level_sub.save_current_level():
        log("Saved %s" % LEVEL_PATH)
        return True

    warn("save_current_level failed")
    return False


if build():
    log("Level ready. Launch with 'Play Living City.cmd'.")
else:
    unreal.log_error("[LivingCity] Level generation FAILED - see the warnings above.")
