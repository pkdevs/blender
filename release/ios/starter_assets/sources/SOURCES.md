# Sources

Every file here is CC0 (public domain). All were downloaded on 2026-10-07.

## Human base meshes

`human_base_meshes.blend` holds six plain meshes extracted from Blender Studio's Human Base
Meshes bundle by `extract_human_base_meshes.py` (see its docstring to redo it). The bundle itself
isn't stored here.

| Original | URL | Author | License |
| --- | --- | --- | --- |
| `human-base-meshes-bundle-v1.4.1.zip` (`human_base_meshes_bundle.blend`) | https://www.blender.org/download/demo-files/ (direct: https://mirror.blender.org/demo/asset-bundles/human-base-meshes/human-base-meshes-bundle-v1.4.1.zip) | Blender Studio and community contributions | CC0 |

| Mesh | From the bundle's |
| --- | --- |
| Human Male Realistic | Body Male - Realistic (multires base level, with eyes) |
| Human Female Realistic | Body Female - Realistic (multires base level, with eyes) |
| Human Male Stylized | Body Male - Stylized (with eyes) |
| Human Female Stylized | Body Female - Stylized (with eyes) |
| Human Hand Realistic | Hand - Realistic (multires level 1, turned fingers up) |
| Human Head Realistic | Head (Sculpting) - Realistic (multires level 1, with eyeballs) |

## Scanned textures

1K JPG maps from [Poly Haven](https://polyhaven.com/license), whose assets are all CC0. Each
folder in `textures/` is named after the Poly Haven asset and keeps its original file names.
Height maps are used for bump instead of normal maps, since the materials use Box projection
without UVs.

| Folder | Files | Asset page | Author |
| --- | --- | --- | --- |
| `red_brick` | `red_brick_diff_1k.jpg`, `red_brick_rough_1k.jpg`, `red_brick_disp_1k.jpg` | https://polyhaven.com/a/red_brick | Rob Tuytel |
| `cobblestone_floor_04` | `cobblestone_floor_04_diff_1k.jpg`, `cobblestone_floor_04_rough_1k.jpg`, `cobblestone_floor_04_disp_1k.jpg` | https://polyhaven.com/a/cobblestone_floor_04 | Rob Tuytel |
| `bark_brown_02` | `bark_brown_02_diff_1k.jpg`, `bark_brown_02_rough_1k.jpg`, `bark_brown_02_disp_1k.jpg` | https://polyhaven.com/a/bark_brown_02 | Rob Tuytel |
| `damaged_plaster` | `damaged_plaster_diff_1k.jpg`, `damaged_plaster_rough_1k.jpg`, `damaged_plaster_disp_1k.jpg` | https://polyhaven.com/a/damaged_plaster | Amal Kumar |
| `fabric_pattern_05` | `fabric_pattern_05_col_01_1k.jpg`, `fabric_pattern_05_rough_1k.jpg`, `fabric_pattern_05_ao_1k.jpg` | https://polyhaven.com/a/fabric_pattern_05 | Rob Tuytel |
| `rocks_ground_02` | `rocks_ground_02_col_1k.jpg`, `rocks_ground_02_rough_1k.jpg`, `rocks_ground_02_height_1k.jpg` | https://polyhaven.com/a/rocks_ground_02 | Rob Tuytel |

Files were fetched through the Poly Haven API (`https://api.polyhaven.com/files/<asset>`), from
`https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/<asset>/<file>`.
