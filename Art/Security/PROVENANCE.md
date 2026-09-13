# Original security vehicles

Vesper hover cruiser and Sentinel attack drone are original project-authored meshes.
`Scripts/bootstrap_voyager_security.py` reproduces the centimetre-scale source OBJ files
and imports three authored LODs into `/Game/Security`. It reuses the project's Kestrel mesh
authoring utilities and original PBR material parents; no vehicle model or paid Fab asset
was downloaded. These are fictional designs, without a licensed vehicle brand.

Vesper has a shaped body and cabin, raked windows, door seams, mirrors, light bar,
headlights and four ducted lift pods. Sentinel has four ducted fans, a sensor and weapon.
The car uses 10,852 / 7,300 / 3,908 triangles; the drone uses 8,928 / 4,836 / 2,464.
Exact source hashes, material slots and bounds are recorded in `meshes.json`.

The game supplies authoritative motion and hit detection. These meshes are visual assets,
not an aerodynamic simulation, driveable civilian car or articulated vehicle interior.
