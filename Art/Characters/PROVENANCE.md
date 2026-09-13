# Riftbound humanoid characters

Prepared on 2026-09-13 for the public Riftbound repository. These are actual
skinned humanoid meshes with authored anatomy, fitted clothing, shoes, hair,
eyes and detailed skin maps. They replace the assembled primitive NPC bodies.

## CC0 source assets

The MakeHuman Community explicitly releases its core graphical assets under
CC0 1.0, including the base mesh, targets, textures, clothes and poses. This
permission covers the original files and scripted derivatives, so these source
assets and the imported Unreal packages may be redistributed with this project.

- Official license: <https://static.makehumancommunity.org/about/license.html>
- Repository asset policy: <https://github.com/makehumancommunity/makehuman/blob/master/LICENSE.md>
- Core asset pack and per-asset CC0 listing: <https://static.makehumancommunity.org/assets/assetpacks/makehuman_system_assets.html>
- CC0 legal text: <https://creativecommons.org/publicdomain/zero/1.0/>

Source assets include the hm08 base mesh; default rig and skin weights; adult
shape targets; high-poly eyes with a brown iris map; three adult skin maps;
male_casualsuit03, female_casualsuit01 and male_worksuit01 outfits; shoes01;
and short01/bob01 hair. The files credit the MakeHuman Team, Data Collection AB,
Joel Palmius and Jonas Hauquier; original credit headers are retained.

`sources.json` records the exact upstream revision, direct source URLs, archive
member names, CRC values, byte counts and SHA256 hashes. The selected archive
members are read from the official pack with HTTP Range requests; the complete
267 MB pack is not required or distributed.

MakeHuman's application code uses AGPL, but no MakeHuman program logic is copied
into the game or pipeline. The accompanying Python code is project-authored.
No Epic mannequin, MetaHuman, Fab, Mixamo, paid marketplace or restricted raw
character content is included.

## Project-authored conversion and animation

`Scripts/bootstrap_voyager_characters.py` fits the CC0 clothes/body parts to
three adult shapes through their published vertex correspondences. Covered
body faces are removed. Skin weights transfer through the same correspondence;
small facial and toe bones merge to retained parents, leaving 77 crowd bones
and four influences per vertex. Models use a 178 cm height and a ground pivot.

The Idle, Walk, Run, Talk and Death clips are newly authored by this project.
Death is a non-graphic timed collapse; it does not require networked ragdolls.
Each collapse keyframe uses the actual skinned body, clothing and shoes to keep
2 cm of ground clearance, with lowered resting shoulders.
No downloaded animation or motion-capture performance is used.

Unreal imports the generated glTF/bin files as skeletal meshes and animation
sequences. Native skeletal reduction creates three LODs. Materials provide
streamed skin/cloth textures, subtle skin scattering, normal-mapped clothing,
masked hair and separate eye shading, with runtime tint controls for variety.

## Reproduction

1. Run `python Scripts/bootstrap_voyager_characters.py --prepare` with NumPy and
   SciPy available to fetch the selected sources and regenerate the models.
2. Run the same script through Unreal Editor's `-ExecutePythonScript` option.
3. Inspect `Saved/VoyagerCharactersReport.json` for imported mesh/animation
   paths, actual assigned material and texture paths, dimensions and LOD vertex
   counts. Assets keep their stable Interchange import paths to make reimports
   repeatable without redirector collisions.

`--fetch` refreshes source data only; `--geometry` regenerates meshes and clips
from the existing local sources without network access. Runtime play is offline.

Passing `-RiftCharacterMaterialsOnly` to the Unreal invocation repairs the saved
mesh material assignments without reimporting geometry, textures or animations.
Passing `-RiftCharacterAudit` performs a read-only reload and verifies the saved
material assignments and on-disk texture dependencies. It only updates the
report under `Saved/` and does not modify content packages.
`-RiftCharacterClipsOnly` updates generated motion through Interchange while
reusing existing textures and master materials, regenerates the same mesh LODs
and rebinds the real materials. It also removes the known early unused character
output only after AssetRegistry confirms that no assets reference it.
