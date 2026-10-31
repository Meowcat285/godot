#!/usr/bin/env python3
"""Prints the Spherical Gaussian lightmap basis constants used by
`lm_sg_inc.glsl` (bake) and `servers/rendering/renderer_rd/shaders/lightmap_sg_inc.glsl` (runtime).

Run it again and paste the output into both files if the lobe layout or sharpness changes.
Requires numpy.
"""

import numpy as np

SHARPNESS = 4.0


def lobe_axes():
    # Icosahedron vertices, rotated so one lobe points straight up (+Y).
    axes = [(0.0, 1.0, 0.0)]
    y = 1.0 / np.sqrt(5.0)
    r = 2.0 / np.sqrt(5.0)
    for k in range(5):
        t = np.radians(72.0 * k)
        axes.append((r * np.cos(t), y, r * np.sin(t)))
    for k in range(5):
        t = np.radians(72.0 * k + 36.0)
        axes.append((r * np.cos(t), -y, r * np.sin(t)))
    axes.append((0.0, -1.0, 0.0))
    return np.array(axes)


def gram_matrix(axes, sharpness):
    # Integral over the sphere of the product of two lobes with unit amplitude.
    count = len(axes)
    m = np.zeros((count, count))
    for i in range(count):
        for j in range(count):
            d = sharpness * np.linalg.norm(axes[i] + axes[j])
            m[i, j] = 4.0 * np.pi * np.exp(-2.0 * sharpness) * (np.sinh(d) / d if d > 1e-6 else 1.0)
    return m


def main():
    axes = lobe_axes()
    inv = np.linalg.inv(gram_matrix(axes, SHARPNESS))
    print(f"#define SG_LOBE_COUNT {len(axes)}")
    print(f"#define SG_SHARPNESS {SHARPNESS:.1f}")
    print()
    print("const vec3 sg_lobe_axes[SG_LOBE_COUNT] = vec3[](")
    print(",\n".join(f"\t\tvec3({a[0]:.8f}, {a[1]:.8f}, {a[2]:.8f})" for a in axes) + ");")
    print()
    print("// Inverse of the lobes' Gram matrix, row-major.")
    print("const float sg_inverse_gram[SG_LOBE_COUNT * SG_LOBE_COUNT] = float[](")
    rows = [", ".join(f"{v:.8f}" for v in row) for row in inv]
    print(",\n".join(f"\t\t{row}" for row in rows) + ");")


if __name__ == "__main__":
    main()
