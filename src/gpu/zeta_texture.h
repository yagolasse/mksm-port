/* Forced include for xemu's pgraph/gl/texture.c only (CMakeLists.txt): xemu's file stays
 * unmodified, its surface-to-texture calls go to zeta_texture.c, which handles a depth surface
 * sampled as a depth texture on the GPU and passes everything else to xemu's functions (plan
 * 2.7c9b). The renames also apply to renderer.h's prototypes as texture.c includes them. */
#ifndef MKSM_ZETA_TEXTURE_H
#define MKSM_ZETA_TEXTURE_H

#define pgraph_gl_check_surface_to_texture_compatibility mksm_gl_check_surface_to_texture_compatibility
#define pgraph_gl_render_surface_to_texture mksm_gl_render_surface_to_texture

#endif
