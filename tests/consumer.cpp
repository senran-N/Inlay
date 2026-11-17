#include <inlay/inlay.h>
int main(){return inlay_doc_config_default().initial_blocks && inlay_math_desc_default().font_size_px>0 && inlay_diagram_desc_default().rank_gap_px>0 ? 0 : 1;}
