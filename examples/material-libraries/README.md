# Starter surface library

`starter-surfaces.gltf` is a self-contained, original collection of 24 uniform metallic-roughness starting points for game props. It uses no third-party images or external files. The names suggest appearances, not measured scans. There is no wood grain, weave, weathering, normal detail, or texture variation; supply your own licensed maps and tune lighting and factors for production assets. It does not simulate transparent glass, refraction, or layered surfaces.

In Cartographer, save the project first. In the material panel, choose **Add 24 starter surface presets**. The same definitions are bundled in the desktop application, so no file browsing or separate download is needed. Alternatively, check the ownership/reuse confirmation, choose **Import GLTF/GLB material library...**, and select this file. The importer appends a source digest to each material name and prevents importing the identical file twice into a project. Assign a material to the desired mesh region. These presets are double-sided because the current material importer requires that setting.

Colors are glTF linear base-color factors, with metallic and roughness values. The file is provided under the repository license. It contains only material definitions, not geometry or bitmap textures.
