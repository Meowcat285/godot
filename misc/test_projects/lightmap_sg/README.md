# Spherical Gaussian lightmap test project

Compares Spherical Gaussian (SG) lightmaps with L1 spherical harmonics lightmaps and real-time lighting.

- `sg.tscn`: directional lightmap with `use_spherical_gaussians` enabled.
- `sh.tscn`: regular directional lightmap, with the old `specular_intensity` approximation set to 1.0.

Both scenes instance `room.tscn`: a room lit by a warm omni light, a cool spot light and a sun through an
opening, with metallic (front) and dielectric (back) spheres from smooth (left) to rough (right). All lights
use the Static bake mode. Textures are procedural (`NoiseTexture2D`), so nothing is downloaded.

## Usage

1. Bake both scenes: open the project, select `LightmapGI` in each scene and click **Bake Lightmaps**, or run
   `godot --editor --path . -- --auto-bake` to bake both and quit.
2. Run a scene. Press <kbd>1</kbd> for the lightmap, <kbd>2</kbd> for real-time lights only (a reference for
   highlights, without bounce lighting). Drag with the left mouse button to orbit.
3. To save a screenshot and quit: `godot --path . sg.tscn -- --capture=/tmp/sg.png` (add `--realtime` for mode 2).

`build_scenes.gd` regenerates the scenes: `godot --headless --path . --script build_scenes.gd`.
