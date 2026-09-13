# Riftbound nature art and redistribution

Prepared for Riftbound / Voyager on 2026-09-13. This directory contains source
art used by `Scripts/bootstrap_voyager_nature.py`; imported Unreal assets live
under `Content/Nature`. Runtime play is entirely offline.

## Third-party source artwork: Poly Haven, CC0 1.0

The following photographed surface/plant maps and two plant meshes were obtained
from Poly Haven's own public API and asset download service. Poly Haven states
that its assets are CC0 and may be modified, used commercially and redistributed,
including within another product or shared work. This permits committing these
source files and the derived Unreal assets to the public Riftbound repository.
No paid, vaulted, Fab, Megascans, marketplace or subscription asset is included.

- License statement: https://polyhaven.com/license
- CC0 1.0: https://creativecommons.org/publicdomain/zero/1.0/
- Public API terms: https://polyhaven.com/our-api

Source assets and contributors:

- [Pine Bark](https://polyhaven.com/a/pine_bark): photographed bark albedo,
  DirectX normal and roughness maps.
- [Tree Small 02](https://polyhaven.com/a/tree_small_02): scanned broadleaf
  albedo, DirectX normal, roughness and opacity atlas. Original complete tree
  geometry is not included or represented as our authored tree geometry.
- [Pine Sapling Small](https://polyhaven.com/a/pine_sapling_small): photographed
  needle/twig albedo, DirectX normal and roughness atlas.
- [Grass Medium 01](https://polyhaven.com/a/grass_medium_01): photographed grass
  maps plus selected small/tall clump mesh variants with original UV mapping.
- [Fern 02](https://polyhaven.com/a/fern_02): photographed fern maps plus two
  frond mesh variants with original UV mapping.
- [Leafy Grass](https://polyhaven.com/a/leafy_grass): photographed ground albedo,
  DirectX normal and roughness maps.
- [Rock Boulder Cracked](https://polyhaven.com/a/rock_boulder_cracked):
  photographed weathered stone albedo, DirectX normal and roughness maps.

Exact author names returned by Poly Haven, direct download URLs, upstream MD5,
downloaded byte counts, and SHA256 hashes are retained in `sources.json`.
Only source assets are included; Poly Haven logos, example renders and website
artwork are not redistributed. The asset credits are retained here voluntarily.
The build utility uses an identifying User-Agent and explicitly credits Poly
Haven; the game does not embed or call the live API.

## Original / adapted geometry and materials

The branched broadleaf tree, conifer, dry shrub and weathered boulder geometry
were authored procedurally for this project by the accompanying source script.
They are new meshes; they are not the multi-million-triangle Poly Haven tree
models. Photographed leaf/twig maps provide fine surface detail and alpha-cut
leaf shapes. These project-authored mesh outputs are made available as CC0 1.0.

The grass and fern meshes are CC0 derivatives of the credited source geometry:
selected clumps/fronds were rotated, combined, root-centered, converted from
metres/Y-up into centimetres/Z-up, and normalized to practical game dimensions.

Each combined static mesh has three separate authored source LODs. Distant LODs
reduce ring counts and foliage density while preserving principal branches.
Grass LODs retain every blade in the same photographed clump; longitudinal
outline reduction preserves its height and footprint instead of substituting
a sparse tuft. Conifer LODs broaden pointed needle proxies to preserve crown
coverage when individual needles become smaller than a screen pixel.
All meshes use a root pivot at Z=0 and +Z growth direction. `meshes.json` records
source triangles, bounds, material slots and hashes for each LOD.

The custom Unreal materials add two-sided foliage lighting, subtle wind,
per-instance tint variation, dithered LOD transitions, and masked instance fade.
The terrain material uses photographed ground/rock maps in continuous triplanar
projection, with world-space surface-gradient normal mapping, radial slope
masking, biome tints and polar snow. No downloaded height map changes gameplay
collision or terrain geometry.

Photographed terrain albedo remains the main color source: the material's
`BiomeTintStrength` defaults to 0.28, limiting tint modulation to 0.902-1.098
per channel. Rock receives less than half that tint strength. A value of zero
preserves unmodified scan color, while snow is a separate polar layer.
Between 500 metres and 3 kilometres, scanned land albedo fades to `FarLandTint`
with broad noise variation representing aggregate vegetation color. Exposed
rock and snow remain independent layers above this distance blend.

## Reproduction

1. Run `python Scripts/bootstrap_voyager_nature.py --prepare` to fetch verified
   sources and regenerate the OBJ/MTL LOD files.
2. Run the same script via Unreal Editor's `-ExecutePythonScript` option to
   import materials, textures and all mesh LODs.
3. Inspect `Saved/VoyagerNatureReport.json` for asset paths, imported dimensions,
   slot contracts and validation results.

Use `-NatureAudit` with the Unreal script invocation to check all 18 imported
LODs for bounds, triangle counts and material slots, then compile materials.
Use `-NatureMaterialsOnly` to refresh the eight materials without reimporting
textures or meshes. Both modes retain the existing verification report.
Use `-NatureLODRefresh` for the grass/conifer distant silhouette update; it
imports only LOD1/LOD2, leaves LOD0 intact, and runs the read-only asset audit.

No Python package, DCC application or plugin download is required by this
pipeline. Source meshes and PBR maps are retained so changing geometry or
materials does not require downloading the assets again.
