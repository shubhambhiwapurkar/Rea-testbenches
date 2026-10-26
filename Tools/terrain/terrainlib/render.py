"""Preview renders for docs: hypsometric-tinted hillshade with contours, and layer composites."""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from .analysis import resize
from .layers import LAYER_COLOURS

# Hypsometric ramp keyed on absolute altitude (m): lowland green -> tan -> brown -> rock grey
# -> snow white. Absolute keys keep colours comparable between mountains.
HYPSO_STOPS = [
    (0, (70, 115, 62)), (1000, (112, 150, 82)), (2000, (168, 172, 108)),
    (3000, (190, 166, 120)), (4000, (168, 134, 98)), (5000, (146, 124, 108)),
    (5800, (166, 162, 160)), (6800, (212, 213, 218)), (8000, (244, 246, 250)),
    (9000, (255, 255, 255)),
]
CONTOUR_RGB = np.array([60, 40, 30], dtype=np.float32)


@dataclass
class Marker:
    label: str
    col: float  # fractional pixel position in the full-resolution heightmap
    row: float
    kind: str = "dot"  # dot | summit


@dataclass
class PreviewInfo:
    title: str
    pixel_m: float  # full-resolution pixel size
    markers: list[Marker] = field(default_factory=list)
    footer: str = ""


def _font(size: int) -> ImageFont.ImageFont:
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def hillshade(h: np.ndarray, px: float, azimuth: float = 315.0, altitude: float = 45.0,
              exaggeration: float = 1.0) -> np.ndarray:
    """Lambertian hillshade in [0, 1] (row 0 = north)."""
    d_row, d_col = np.gradient(h * exaggeration, px)
    dz_east, dz_north = d_col, -d_row
    az, alt = np.radians(azimuth), np.radians(altitude)
    light = np.array([np.sin(az) * np.cos(alt), np.cos(az) * np.cos(alt), np.sin(alt)])
    norm = np.sqrt(dz_east ** 2 + dz_north ** 2 + 1.0)
    shade = (-dz_east * light[0] - dz_north * light[1] + light[2]) / norm
    return np.clip(shade, 0.0, 1.0).astype(np.float32)


def hypsometric_rgb(h: np.ndarray) -> np.ndarray:
    keys = np.array([k for k, _ in HYPSO_STOPS], dtype=np.float32)
    cols = np.array([c for _, c in HYPSO_STOPS], dtype=np.float32)
    return np.stack([np.interp(h, keys, cols[:, i]) for i in range(3)], axis=-1)


def contour_mask(h: np.ndarray, interval: float) -> tuple[np.ndarray, np.ndarray]:
    """Boolean masks of contour pixels (every interval) and index contours (every 2x)."""
    level = np.floor(h / interval).astype(np.int32)
    edge = np.zeros(h.shape, dtype=bool)
    edge[:, :-1] |= level[:, :-1] != level[:, 1:]
    edge[:-1, :] |= level[:-1, :] != level[1:, :]
    hi = np.maximum(level, np.roll(level, -1, 1))
    hi = np.maximum(hi, np.roll(level, -1, 0))
    index = edge & (hi % 2 == 0)
    return edge, index


def downsample(h: np.ndarray, width: int) -> tuple[np.ndarray, float]:
    """Area-preserving downsample to `width` pixels; returns (array, factor)."""
    if h.shape[1] <= width:
        return h.astype(np.float32), 1.0
    factor = h.shape[1] / width
    out_shape = (int(round(h.shape[0] / factor)), width)
    return resize(h, out_shape, order=1), factor


def _draw_markers(img: Image.Image, info: PreviewInfo, factor: float) -> None:
    draw = ImageDraw.Draw(img)
    font = _font(max(12, img.width // 75))
    for m in info.markers:
        x, y = m.col / factor, m.row / factor
        if not (0 <= x < img.width and 0 <= y < img.height):
            continue
        r = max(4, img.width // 220)
        if m.kind == "summit":
            draw.polygon([(x, y - r * 1.6), (x - r * 1.4, y + r), (x + r * 1.4, y + r)],
                         fill=(200, 30, 30), outline=(255, 255, 255))
        else:
            draw.ellipse([x - r, y - r, x + r, y + r], fill=(20, 90, 200), outline=(255, 255, 255))
        tx, ty = x + r * 1.8, y - r * 1.8
        for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            draw.text((tx + dx, ty + dy), m.label, fill=(255, 255, 255), font=font)
        draw.text((tx, ty), m.label, fill=(10, 10, 10), font=font)


def _north_arrow(draw: ImageDraw.ImageDraw, x: float, y: float, size: float, font) -> None:
    draw.polygon([(x, y), (x - size * 0.45, y + size), (x + size * 0.45, y + size)],
                 fill=(20, 20, 20), outline=(255, 255, 255))
    draw.text((x - size * 0.22, y + size * 1.05), "N", fill=(20, 20, 20), font=font)


def _footer(width: int, height: int, info: PreviewInfo, map_px_m: float,
            ramp: tuple[float, float] | None, swatches: list[tuple[str, tuple]] | None) -> Image.Image:
    bar = Image.new("RGB", (width, height), (250, 250, 248))
    draw = ImageDraw.Draw(bar)
    font = _font(max(12, width // 90))
    small = _font(max(11, width // 110))
    pad = 12
    draw.text((pad, 8), info.title, fill=(20, 20, 20), font=font)
    # scale bar: a round number of km close to 1/5 of the width
    span_m = map_px_m * width / 5
    km = max(1, int(10 ** np.floor(np.log10(span_m / 1000))))
    for mult in (5, 2, 1):
        if mult * km * 1000 <= span_m:
            km *= mult
            break
    bar_px = km * 1000 / map_px_m
    y0 = height - 22
    draw.rectangle([pad, y0, pad + bar_px, y0 + 6], fill=(20, 20, 20))
    draw.text((pad + bar_px + 6, y0 - 6), f"{km} km", fill=(20, 20, 20), font=small)
    x_ramp = int(width * 0.36)
    if ramp is not None:
        lo, hi = ramp
        ramp_w = int(width * 0.42)
        vals = np.linspace(lo, hi, ramp_w)
        rgb = hypsometric_rgb(vals[None, :])[0].astype(np.uint8)
        strip = Image.fromarray(np.repeat(rgb[None], 14, axis=0))
        bar.paste(strip, (x_ramp, y0 - 8))
        for tick in range(int(np.ceil(lo / 1000) * 1000), int(hi) + 1, 1000):
            tx = x_ramp + (tick - lo) / (hi - lo) * ramp_w
            draw.line([(tx, y0 - 10), (tx, y0 + 7)], fill=(20, 20, 20))
            draw.text((tx - 14, y0 - 26), f"{tick}", fill=(20, 20, 20), font=small)
        draw.text((x_ramp + ramp_w + 8, y0 - 6), "m (contours 500 m)", fill=(20, 20, 20), font=small)
    if swatches:
        x = x_ramp
        for name, col in swatches:
            draw.rectangle([x, y0 - 6, x + 16, y0 + 8], fill=tuple(col), outline=(40, 40, 40))
            draw.text((x + 21, y0 - 6), name, fill=(20, 20, 20), font=small)
            x += 26 + int(draw.textlength(name, font=small)) + 14
    if info.footer:
        draw.text((pad, 30), info.footer, fill=(90, 90, 90), font=small)
    return bar


def _compose(map_rgb: np.ndarray, info: PreviewInfo, factor: float,
             ramp: tuple[float, float] | None, swatches) -> Image.Image:
    img = Image.fromarray(np.clip(map_rgb, 0, 255).astype(np.uint8))
    _draw_markers(img, info, factor)
    draw = ImageDraw.Draw(img)
    size = max(18, img.width // 45)
    _north_arrow(draw, img.width - size * 1.4, size * 0.6, size, _font(max(12, img.width // 80)))
    footer = _footer(img.width, 92, info, info.pixel_m * factor, ramp, swatches)
    out = Image.new("RGB", (img.width, img.height + footer.height), (255, 255, 255))
    out.paste(img, (0, 0))
    out.paste(footer, (0, img.height))
    return out


def render_hillshade(h: np.ndarray, info: PreviewInfo, width: int = 1600,
                     contour_interval: float = 500.0) -> Image.Image:
    small, factor = downsample(h, width)
    px = info.pixel_m * factor
    shade = hillshade(small, px)
    rgb = hypsometric_rgb(small) * (0.30 + 0.70 * shade)[..., None]
    edge, index = contour_mask(small, contour_interval)
    rgb[edge] = rgb[edge] * 0.55 + CONTOUR_RGB * 0.45
    rgb[index] = rgb[index] * 0.2 + CONTOUR_RGB * 0.8
    lo = float(np.floor(small.min() / 500.0) * 500.0)
    hi = float(np.ceil(small.max() / 500.0) * 500.0)
    return _compose(rgb, info, factor, (lo, hi), None)


def render_layers(h: np.ndarray, weights: dict[str, np.ndarray], info: PreviewInfo,
                  width: int = 1600) -> Image.Image:
    small, factor = downsample(h, width)
    shade = hillshade(small, info.pixel_m * factor)
    rgb = np.zeros(small.shape + (3,), dtype=np.float32)
    for name, w in weights.items():
        ws, _ = downsample(w.astype(np.float32) / 255.0, width)
        rgb += ws[..., None] * np.array(LAYER_COLOURS[name], dtype=np.float32)
    rgb *= (0.40 + 0.60 * shade)[..., None]
    swatches = [(n, LAYER_COLOURS[n]) for n in weights]
    return _compose(rgb, info, factor, None, swatches)


# Colours that must survive palette quantisation exactly (markers, text, legend swatches).
_FIXED_COLOURS = [(255, 255, 255), (0, 0, 0), (10, 10, 10), (20, 20, 20), (200, 30, 30),
                  (20, 90, 200), (250, 250, 248), (90, 90, 90), *LAYER_COLOURS.values()]


def _quantize(img: Image.Image, colors: int) -> Image.Image:
    """Adaptive palette for the map plus fixed overlay colours, Floyd-Steinberg dithered."""
    n_free = colors - len(_FIXED_COLOURS)
    base = img.quantize(colors=n_free, method=Image.Quantize.MEDIANCUT)
    pal = base.getpalette()[: 3 * n_free]
    pal += [v for rgb in _FIXED_COLOURS for v in rgb]
    pal_img = Image.new("P", (1, 1))
    pal_img.putpalette(pal)
    return img.convert("RGB").quantize(palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG)


def save_compact_png(img: Image.Image, path, max_bytes: int | None = None) -> int:
    """Save as an adaptive-palette PNG (small, still smooth for shaded relief)."""
    for colors in (256, 192, 128):
        _quantize(img, colors).save(path, optimize=True)
        size = path.stat().st_size
        if not max_bytes or size <= max_bytes:
            break
    return size
