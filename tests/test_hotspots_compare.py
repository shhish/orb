"""The image receipt must count changed visible pixels without corner dilution."""

import math

import pytest
from hotspots_compare import measure
from PIL import Image


def test_metrics_use_only_the_supplied_visible_mask():
    first = Image.new("RGB", (2, 1), (0, 0, 0))
    second = Image.new("RGB", (2, 1), (255, 255, 255))
    second.putpixel((0, 0), (3, 0, 0))
    mask = Image.new("L", (2, 1))
    mask.putpixel((0, 0), 255)
    result = measure(first, second, mask)
    assert result["pixels"] == 1
    assert result["mae_rgb"] == 1
    assert result["changed_percent"] == 100
    assert result["max_channel_error"] == 3
    assert result["psnr_db"] == pytest.approx(10 * math.log10(255 * 255 / 3))
    assert measure(first, first, mask)["psnr_db"] is None
    with pytest.raises(ValueError, match="empty"):
        measure(first, second, Image.new("L", (2, 1)))
