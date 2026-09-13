# Original field supplies and procedural furniture

`Source/Riftbound/VoyagerItemVisuals.cpp` generates seven original centimetre-scale
models: raw meat, a cooked meal tray, folded hide, bone, a medkit, an inert demolition
charge housing and an energy cartridge. The source is the reproducible art asset.
No downloaded model, marketplace pack or image texture was used for this addition.

Each model uses one procedural mesh component, at most three opaque material sections
and at most 1,408 triangles. There is no per-item Tick, collision or skeletal mesh.
They use the existing project-authored Kestrel interior material, with the project
surface material and Epic's Engine BasicShape material as fallbacks. These models
have original geometry and material parameters; they are not photogrammetry assets.

`VoyagerFurnitureLayout.h` composes original chairs and workstations using the existing
instanced Engine BasicShape cube. Monitor housings are opaque and the panel emits only
on the user-facing side. Keyboards and mice rest on their associated tabletop. Epic's
basic shape meshes remain engine content, not project-authored mesh assets.

Held models are presentation of owned inventory. The same charge mesh is attached to
building surfaces through the existing timed demolition action. None of this art
controls inventory, health, structural damage or authoritative simulation outcomes.
