#include <inlay/inlay_c.h>
#include <cmath>
#include <cstring>
namespace {
struct Graph {EtchStr source;uint32_t pos,capacity;InlayDiagramNode* nodes;InlayDiagramEdge* edges;uint32_t node_count,edge_count;bool valid,lr;};
void spaces(Graph* g){while(g->pos<g->source.length && (g->source.chars[g->pos]==' ' || g->source.chars[g->pos]=='\t' || g->source.chars[g->pos]=='\r'))++g->pos;}
bool word(char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_';}
bool equal(EtchStr a,EtchStr b){return a.length==b.length && !std::memcmp(a.chars,b.chars,a.length);}
uint32_t node(Graph* g) {
 spaces(g);const uint32_t start=g->pos;while(g->pos<g->source.length && word(g->source.chars[g->pos]))++g->pos;
 if(start==g->pos){g->valid=false;return 0;}
 const EtchStr id={g->source.chars+start,g->pos-start,false};uint32_t index=0;
 while(index<g->node_count && !equal(g->nodes[index].id,id))++index;
 if(index==g->node_count){if(index==256){g->valid=false;return 0;}g->nodes[index]={};g->nodes[index].id=id;g->nodes[index].label=id;++g->node_count;}
 spaces(g);
 if(g->pos<g->source.length && (g->source.chars[g->pos]=='[' || g->source.chars[g->pos]=='(' || g->source.chars[g->pos]=='{')) {
  const char open=g->source.chars[g->pos++],close=open=='[' ? ']' : open=='(' ? ')' : '}';const uint32_t label=g->pos;
  while(g->pos<g->source.length && g->source.chars[g->pos]!=close && g->source.chars[g->pos]!='\n')++g->pos;
  if(g->pos==g->source.length || g->source.chars[g->pos]!=close){g->valid=false;return index;}
  g->nodes[index].label={g->source.chars+label,g->pos-label,false};++g->pos;
 }
 return index;
}
InlayDiagramResult layout(EtchStr source,const InlayDiagramDesc* input) {
 auto* ctx=etch_current_context();
 if(!ctx || !etch_frame_in_progress(ctx) || !input || !source.chars || source.length>16384 ||
 !std::isfinite(input->pos.x) || !std::isfinite(input->pos.y) || !std::isfinite(input->font_size_px) || input->font_size_px<6 || input->font_size_px>128 ||
 !std::isfinite(input->node_gap_px) || input->node_gap_px<0 || input->node_gap_px>4096 || !std::isfinite(input->rank_gap_px) || input->rank_gap_px<0 || input->rank_gap_px>4096 || (input->canvas_id && (!std::isfinite(input->viewport_width_px) || !std::isfinite(input->viewport_height_px) || input->viewport_width_px<1 || input->viewport_height_px<1)))return {};
 const uint32_t capacity=256;
 auto* nodes=static_cast<InlayDiagramNode*>(etch_frame_arena_alloc(ctx,size_t(capacity)*sizeof(InlayDiagramNode),alignof(InlayDiagramNode)));
 auto* edges=static_cast<InlayDiagramEdge*>(etch_frame_arena_alloc(ctx,size_t(1024)*sizeof(InlayDiagramEdge),alignof(InlayDiagramEdge)));
 if(!nodes || !edges)return {};
 Graph g={source,0,capacity,nodes,edges,0,0,true,false};spaces(&g);
 const uint32_t header=g.pos;
 while(g.pos<source.length && word(source.chars[g.pos]))++g.pos;
 const EtchStr kind={source.chars+header,g.pos-header,false};
 if(equal(kind,ETCH_STR_LIT("sequenceDiagram"))) {
  while(g.valid && g.pos<source.length) {
   spaces(&g);if(g.pos==source.length)break;if(source.chars[g.pos]=='\n' || source.chars[g.pos]==';'){++g.pos;continue;}
   if(g.pos+12<=source.length && !std::memcmp(source.chars+g.pos,"participant ",12)) {
    g.pos+=12;const uint32_t who=node(&g);spaces(&g);
    if(g.pos+3<=source.length && !std::memcmp(source.chars+g.pos,"as ",3)) {g.pos+=3;const uint32_t name=g.pos;while(g.pos<source.length && source.chars[g.pos]!='\n')++g.pos;nodes[who].label={source.chars+name,g.pos-name,false};}
    continue;
   }
   const uint32_t from=node(&g);spaces(&g);
   if(g.pos+4>source.length || std::memcmp(source.chars+g.pos,"->>",3)){g.valid=false;break;}
   g.pos+=3;const uint32_t to=node(&g);spaces(&g);
   if(!g.valid || g.pos==source.length || source.chars[g.pos++]!=':' || g.edge_count==1024){g.valid=false;break;}
   spaces(&g);const uint32_t label=g.pos;while(g.pos<source.length && source.chars[g.pos]!='\n')++g.pos;
   edges[g.edge_count++]={from,to,false,{source.chars+label,g.pos-label,false}};
  }
  if(!g.valid || !g.node_count)return {};
  float width=0,max_message=0;
  for(uint32_t i=0;i<g.edge_count;++i)max_message=std::fmax(max_message,etch_measure_text(edges[i].label,ETCH_FONT_SANS,input->font_size_px,0).x+24);
  for(uint32_t i=0;i<g.node_count;++i){const auto size=etch_measure_text(nodes[i].label,ETCH_FONT_SANS,input->font_size_px,0);nodes[i].bounds={input->pos.x+width,input->pos.y,std::fmax(size.x+32,max_message),input->font_size_px*1.5f+24};width+=nodes[i].bounds.w+input->node_gap_px;}
  const float height=input->font_size_px*1.5f+24+float(g.edge_count+1)*input->rank_gap_px;
  for(uint32_t i=0;i<g.edge_count;++i)if(edges[i].from==edges[i].to){width+=40;break;}
  return {nodes,edges,g.node_count,g.edge_count,width-input->node_gap_px,height,true,false,true};
 }
 if(!equal(kind,ETCH_STR_LIT("flowchart")) && !equal(kind,ETCH_STR_LIT("graph")))return {};
 spaces(&g);const uint32_t direction=g.pos;while(g.pos<source.length && word(source.chars[g.pos]))++g.pos;
 const EtchStr dir={source.chars+direction,g.pos-direction,false};
 g.lr=equal(dir,ETCH_STR_LIT("LR"));if(!g.lr && !equal(dir,ETCH_STR_LIT("TD")) && !equal(dir,ETCH_STR_LIT("TB")))return {};
 while(g.valid && g.pos<source.length) {
  spaces(&g);if(g.pos==source.length)break;
  if(source.chars[g.pos]=='\n' || source.chars[g.pos]==';'){++g.pos;continue;}
  if(g.pos+1<source.length && source.chars[g.pos]=='%' && source.chars[g.pos+1]=='%'){while(g.pos<source.length && source.chars[g.pos]!='\n')++g.pos;continue;}
  uint32_t from=node(&g);
  while(g.valid) {
   spaces(&g);if(g.pos==source.length || source.chars[g.pos]=='\n' || source.chars[g.pos]==';')break;
   if(g.pos+3>source.length || std::memcmp(source.chars+g.pos,"-->",3)){g.valid=false;break;}
   g.pos+=3;const uint32_t to=node(&g);if(!g.valid)break;
   if(g.edge_count==1024){g.valid=false;break;}
   edges[g.edge_count++]={from,to,false,{}};from=to;
  }
 }
 if(!g.valid || !g.node_count)return {};
 // Iterative DFS reverses feedback edges; the remaining orientation is acyclic.
 auto* colors=static_cast<uint8_t*>(etch_frame_arena_alloc(ctx,g.node_count,1));
 auto* stack=static_cast<uint32_t*>(etch_frame_arena_alloc(ctx,size_t(g.node_count)*sizeof(uint32_t),alignof(uint32_t)));
 auto* cursor=static_cast<uint32_t*>(etch_frame_arena_alloc(ctx,size_t(g.node_count)*sizeof(uint32_t),alignof(uint32_t)));
 auto* order=static_cast<uint32_t*>(etch_frame_arena_alloc(ctx,size_t(g.node_count)*sizeof(uint32_t),alignof(uint32_t)));
 auto* bary=static_cast<float*>(etch_frame_arena_alloc(ctx,size_t(g.node_count)*sizeof(float),alignof(float)));
 if(!colors || !stack || !cursor || !order || !bary)return {};
 std::memset(colors,0,g.node_count);std::memset(cursor,0,size_t(g.node_count)*sizeof(uint32_t));uint32_t finished=0;
 for(uint32_t root=0;root<g.node_count;++root)if(!colors[root]) {
  uint32_t depth=1;stack[0]=root;colors[root]=1;
  while(depth) {
   const uint32_t at=stack[depth-1];uint32_t& edge=cursor[at];
   while(edge<g.edge_count && (edges[edge].from!=at || edges[edge].from==edges[edge].to))++edge;
   if(edge==g.edge_count){colors[at]=2;order[finished++]=at;--depth;continue;}
   auto& e=edges[edge++];
   if(colors[e.to]==1){e.reversed=true;continue;}
   if(!colors[e.to]){colors[e.to]=1;stack[depth++]=e.to;}
  }
 }
 uint32_t max_rank=0;
 for(uint32_t reverse=finished;reverse>0;--reverse) {
  const uint32_t at=order[reverse-1];
  for(uint32_t edge=0;edge<g.edge_count;++edge) {
   const auto& e=edges[edge];if(e.from==e.to)continue;
   const uint32_t from=e.reversed ? e.to : e.from,to=e.reversed ? e.from : e.to;
   if(from==at && nodes[to].rank<=nodes[from].rank)nodes[to].rank=nodes[from].rank+1;
  }
 }
 for(uint32_t i=0;i<g.node_count;++i){order[i]=i;max_rank=nodes[i].rank>max_rank ? nodes[i].rank : max_rank;}
 // Stable barycentric sweeps reduce crossings within each rank.
 for(uint32_t pass=0;pass<4;++pass) {
  for(uint32_t i=0;i<g.node_count;++i) {
   float sum=0;uint32_t count=0;
   for(uint32_t edge=0;edge<g.edge_count;++edge) {
    const auto& e=edges[edge];const uint32_t from=e.reversed ? e.to : e.from,to=e.reversed ? e.from : e.to;
    if(to==order[i]){for(uint32_t j=0;j<g.node_count;++j)if(order[j]==from){sum+=float(j);++count;break;}}
   }
   bary[order[i]]=count ? sum/float(count) : float(i);
  }
  for(uint32_t i=1;i<g.node_count;++i){const uint32_t value=order[i];uint32_t j=i;while(j && (nodes[order[j-1]].rank>nodes[value].rank || (nodes[order[j-1]].rank==nodes[value].rank && bary[order[j-1]]>bary[value]))){order[j]=order[j-1];--j;}order[j]=value;}
 }
 float rank_position=0,total_cross=0;
 for(uint32_t rank=0;rank<=max_rank;++rank) {
  float cross=0,rank_size=0;
  for(uint32_t i=0;i<g.node_count;++i) {
   auto& n=nodes[order[i]];if(n.rank!=rank)continue;
   const auto text=etch_measure_text(n.label,ETCH_FONT_SANS,input->font_size_px,0);
   n.bounds={input->pos.x+(g.lr ? rank_position : cross),input->pos.y+(g.lr ? cross : rank_position),text.x+32,input->font_size_px*1.5f+24};
   cross+=(g.lr ? n.bounds.h : n.bounds.w)+input->node_gap_px;
   rank_size=std::fmax(rank_size,g.lr ? n.bounds.w : n.bounds.h);
  }
  total_cross=std::fmax(total_cross,cross>0 ? cross-input->node_gap_px : 0);
  rank_position+=rank_size+(rank<max_rank ? input->rank_gap_px : 0);
 }
 float width=g.lr ? rank_position : total_cross,height=g.lr ? total_cross : rank_position;
 for(uint32_t i=0;i<g.edge_count;++i)if(edges[i].from==edges[i].to)width=std::fmax(width,nodes[edges[i].from].bounds.x-input->pos.x+nodes[edges[i].from].bounds.w+40);
 for(uint32_t i=0;i<g.edge_count;++i)if(edges[i].reversed){if(g.lr)height+=40;else width+=40;break;}
 return {nodes,edges,g.node_count,g.edge_count,width,height,true,g.lr,false};
}
}
extern "C" {
InlayDiagramDesc inlay_diagram_desc_default(void){return {{0,0},16,24,48,0,640,400};}
InlayDiagramResult inlay_diagram_layout(EtchStr source,const InlayDiagramDesc* desc){return layout(source,desc);}
InlayDiagramResult inlay_diagram_view(EtchStr source,const InlayDiagramDesc* input) {
 auto result=layout(source,input);if(!result.valid)return result;
 if(input->canvas_id){auto canvas=etch_canvas_desc_default();canvas.id=input->canvas_id;canvas.width=etch_sizing_fixed(input->viewport_width_px);canvas.height=etch_sizing_fixed(input->viewport_height_px);etch_begin_canvas(&canvas); }
 if(result.sequence)for(uint32_t i=0;i<result.node_count;++i){const auto b=result.nodes[i].bounds;EtchLineDesc line={};line.from={b.x+b.w*.5f,b.y+b.h};line.to={line.from.x,input->pos.y+result.height};line.color=etch_rgba8(75,110,140,255);line.thickness=1;etch_draw_line(&line);}
 for(uint32_t i=0;i<result.edge_count;++i) {
  const auto& edge=result.edges[i];const auto a=result.nodes[edge.from].bounds,b=result.nodes[edge.to].bounds;
  auto bezier=etch_bezier_desc_default();bezier.p0={a.x+a.w*.5f,a.y+a.h};bezier.p3={b.x+b.w*.5f,b.y};
  if(result.horizontal){bezier.p0={a.x+a.w,a.y+a.h*.5f};bezier.p3={b.x,b.y+b.h*.5f};}
  if(result.sequence){const float y=input->pos.y+a.h+float(i+1)*input->rank_gap_px;bezier.p0={a.x+a.w*.5f,y};bezier.p3={b.x+b.w*.5f,y};}
  bezier.p1=result.horizontal ? EtchVec2{(bezier.p0.x+bezier.p3.x)*.5f,bezier.p0.y} : EtchVec2{bezier.p0.x,(bezier.p0.y+bezier.p3.y)*.5f};
  bezier.p2=result.horizontal ? EtchVec2{bezier.p1.x,bezier.p3.y} : EtchVec2{bezier.p3.x,bezier.p1.y};
  if(result.sequence){bezier.p1=bezier.p0;bezier.p2=bezier.p3;}
  if(edge.reversed) {
   if(result.horizontal){const float route=input->pos.y+result.height;bezier.p0={a.x+a.w*.5f,a.y+a.h};bezier.p3={b.x+b.w*.5f,b.y+b.h};bezier.p1={bezier.p0.x,route};bezier.p2={bezier.p3.x,route};}
   else {const float route=input->pos.x+result.width;bezier.p0={a.x+a.w,a.y+a.h*.5f};bezier.p3={b.x+b.w,b.y+b.h*.5f};bezier.p1={route,bezier.p0.y};bezier.p2={route,bezier.p3.y};}
  }
  if(edge.from==edge.to){const float loop_y=result.sequence ? bezier.p0.y : a.y+a.h*.5f,loop_x=result.sequence ? a.x+a.w*.5f : a.x+a.w;bezier.p0={loop_x,loop_y-12};bezier.p3={loop_x,loop_y+12};bezier.p1={loop_x+40,bezier.p0.y};bezier.p2={loop_x+40,bezier.p3.y};}
  bezier.cubic=true;
  bezier.color_start=etch_rgba8(80,190,240,255);bezier.color_end=bezier.color_start;bezier.thickness=2;etch_draw_bezier(&bezier);
  EtchVec2 tangent={bezier.p3.x-bezier.p2.x,bezier.p3.y-bezier.p2.y};
  if(result.sequence && edge.from!=edge.to)tangent={bezier.p3.x-bezier.p0.x,0};
  const float len=std::sqrt(tangent.x*tangent.x+tangent.y*tangent.y);
  if(len>0){tangent.x/=len;tangent.y/=len;EtchVec2 points[]={bezier.p3,{bezier.p3.x-tangent.x*8-tangent.y*4,bezier.p3.y-tangent.y*8+tangent.x*4},{bezier.p3.x-tangent.x*8+tangent.y*4,bezier.p3.y-tangent.y*8-tangent.x*4}};EtchFilledPolygonDesc arrow={};arrow.points=points;arrow.point_count=3;arrow.fill=bezier.color_start;etch_draw_filled_polygon(&arrow);}
  if(edge.label.length){auto text=etch_text_draw_desc_default();text.text=edge.label;text.pos={(bezier.p0.x+bezier.p3.x)*.5f,bezier.p0.y-input->font_size_px*1.5f};text.align_x=ETCH_ALIGN_X_CENTER;text.size=input->font_size_px;text.color=etch_rgba8(210,225,240,255);etch_draw_text(&text);}

 }
 for(uint32_t i=0;i<result.node_count;++i) {
  const auto& n=result.nodes[i];auto rect=etch_rect_draw_desc_default();rect.rect=n.bounds;rect.fill=etch_rgba8(25,40,58,255);rect.border_color=etch_rgba8(75,145,185,255);rect.border_width=1;rect.radius=etch_corners_all(10);rect.continuous_corners=true;etch_draw_rect(&rect);
  auto text=etch_text_draw_desc_default();text.text=n.label;text.pos={n.bounds.x+16,n.bounds.y+12};text.size=input->font_size_px;text.color=etch_rgba8(225,235,245,255);etch_draw_text(&text);
 }
 if(input->canvas_id)etch_end_canvas();
 return result;
}
}
