// Copyright 2026 N-GINN LLC. All rights reserved.

// Keep G-buffer, UID, AO depth/normals and shadow coverage identical.
void clipFoliageAlpha(float alpha)
{
    clip(alpha - 0.5f);
}
