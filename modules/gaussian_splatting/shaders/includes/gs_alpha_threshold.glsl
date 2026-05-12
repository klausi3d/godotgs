#ifndef GS_ALPHA_THRESHOLD_GLSL
#define GS_ALPHA_THRESHOLD_GLSL

// Shared forward-render alpha support contract.
// Matches the common PlayCanvas/SuperSplat threshold for visible splat
// contribution and keeps binning support, raster quadratic rejection, and
// alpha rejection on the same iso-contour.
const float GS_RASTER_ALPHA_THRESHOLD = 1.0 / 255.0;
const float GS_RASTER_ALPHA_REJECT_Q = 11.08252716; // -2.0 * ln(1.0 / 255.0)
const float GS_RASTER_MAX_SIGMA = 3.32904339; // sqrt(GS_RASTER_ALPHA_REJECT_Q)

#endif // GS_ALPHA_THRESHOLD_GLSL
