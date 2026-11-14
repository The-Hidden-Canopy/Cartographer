# Starter surface library

`starter-surfaces.gltf` is a self-contained, original collection of 24 uniform metallic-roughness starting points for game props. It uses no third-party images or external files. The names suggest appearances, not measured scans. There is no wood grain, weave, weathering, normal detail, or texture variation; supply your own licensed maps and tune lighting and factors for production assets. It does not simulate transparent glass, refraction, or layered surfaces.

In Cartographer, save the project first. In the material panel, check the ownership/reuse confirmation and choose **Import GLTF/GLB material library...**, then select this file. The importer appends a source digest to each material name and prevents importing the identical file twice into a project. Assign a material to the desired mesh region. These presets are double-sided because the current material importer requires that setting.

Colors are glTF linear base-color factors, with metallic and roughness values. The file is provided under the repository license. It contains only material definitions, not geometry or bitmap textures.
