#include <inlay/inlay.h>
int main(){const inlay::SurfaceDesc surface=inlay_surface_desc_default();return surface.max_nodes && inlay_doc_config_default().initial_blocks && inlay_math_desc_default().font_size_px>0 && inlay_diagram_desc_default().rank_gap_px>0 ? 0 : 1;}
