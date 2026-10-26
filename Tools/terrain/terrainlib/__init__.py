"""SUMMIT terrain pipeline library: real-world DEM to Unreal Engine 5 Landscape.

Modules
-------
dem      Copernicus GLO-30 tile naming, download cache, mosaic and reprojection.
ue       Unreal Engine Landscape size table and import-setting maths.
detail   Deterministic sub-30 m detail synthesis (ridged noise + thermal erosion).
layers   Paint-layer weightmaps (rock, snow, glacier, scree, grass, dirt).
render   Hillshade / contour / layer-composite preview images.
presets  Peak presets (Tools/terrain/peaks_presets.json).
"""

__version__ = "1.0.0"
