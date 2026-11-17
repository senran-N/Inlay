#include <inlay/inlay_c.h>
#include <cmath>
#include <cstring>
namespace {
enum Kind : uint8_t {Row,Glyph,Fraction,Root,Scripts,Accent,Grid,Delimited};
struct Box {Kind kind;float width,ascent,descent,size;EtchStr text;Box *first,*last,*next,*a,*b,*c;float ax,ay,bx,by,cx,cy;};
struct Parser {EtchStr source;uint32_t pos;bool valid;uint32_t depth;};
Box* make(Parser* p,Kind kind,float size) {
 auto* box=static_cast<Box*>(etch_frame_arena_alloc(etch_current_context(),sizeof(Box),alignof(Box)));
 if(!box){p->valid=false;return nullptr;}*box={};box->kind=kind;box->size=size;return box;
}
Box* glyph(Parser* p,EtchStr text,float size) {
 auto* box=make(p,Glyph,size);if(!box)return nullptr;box->text=text;
 const auto measure=etch_measure_text(text,ETCH_FONT_SANS,size,0);
 const auto metrics=etch_font_metrics(ETCH_FONT_SANS,size);
 box->width=measure.x;box->ascent=metrics.ascent;box->descent=metrics.descent;return box;
}
void append(Box* row,Box* item) {
 if(row->last)row->last->next=item;else row->first=item;row->last=item;
 row->width+=item->width;row->ascent=std::fmax(row->ascent,item->ascent);row->descent=std::fmax(row->descent,item->descent);
}
void spaces(Parser* p){while(p->pos<p->source.length && (p->source.chars[p->pos]==' ' || p->source.chars[p->pos]=='\n' || p->source.chars[p->pos]=='\r' || p->source.chars[p->pos]=='\t'))++p->pos;}
Box* expression(Parser*,float,bool,bool=false,bool=false);
Box* atom(Parser*,float);
Box* atom_impl(Parser* p,float size) {
 spaces(p);if(p->pos==p->source.length){p->valid=false;return nullptr;}
 const auto source=p->source;const uint32_t start=p->pos;const char c=source.chars[p->pos++];
 if(c=='{')return expression(p,size,true);
 if(c=='}'){p->valid=false;return nullptr;}
 if(c!='\\') {
  while(p->pos<source.length && (uint8_t(source.chars[p->pos])&0xc0)==0x80)++p->pos;
  return glyph(p,{source.chars+start,p->pos-start,false},size);
 }
 const uint32_t command=p->pos;
 while(p->pos<source.length && ((source.chars[p->pos]>='a' && source.chars[p->pos]<='z') || (source.chars[p->pos]>='A' && source.chars[p->pos]<='Z')))++p->pos;
 const uint32_t n=p->pos-command;
 const auto is=[&](const char* word){return std::strlen(word)==n && !std::memcmp(source.chars+command,word,n);};
 if(!n){if(p->pos==source.length){p->valid=false;return nullptr;}return glyph(p,{source.chars+p->pos++,1,false},size);}
 if(is("quad") || is("qquad")){auto* box=make(p,Row,size);if(box)box->width=size*(is("qquad") ? 2.f : 1.f);return box;}
 if(is("text") || is("mathrm") || is("operatorname")) {
  spaces(p);if(p->pos==source.length || source.chars[p->pos++]!='{'){p->valid=false;return nullptr;}
  const uint32_t text=p->pos;while(p->pos<source.length && source.chars[p->pos]!='}')++p->pos;
  if(p->pos==source.length){p->valid=false;return nullptr;}
  const uint32_t length=p->pos++-text;return glyph(p,{source.chars+text,length,false},size);
 }
 if(is("begin")) {
  spaces(p);if(p->pos==source.length || source.chars[p->pos++]!='{'){p->valid=false;return nullptr;}
  const uint32_t env=p->pos;while(p->pos<source.length && source.chars[p->pos]!='}')++p->pos;
  const uint32_t env_len=p->pos-env;
  if(p->pos==source.length || !((env_len==6 && !std::memcmp(source.chars+env,"matrix",6)) || (env_len==7 && (!std::memcmp(source.chars+env,"aligned",7) || !std::memcmp(source.chars+env,"pmatrix",7) || !std::memcmp(source.chars+env,"bmatrix",7))))){p->valid=false;return nullptr;}
  const char delimiter=env_len==7 && source.chars[env]!='a' ? source.chars[env] : 0;
  ++p->pos;auto* grid=make(p,Grid,size);if(!grid)return nullptr;
  float widths[64]={},ascents[64]={},descents[64]={};uint32_t row=0,column=0,columns=0;
  while(p->valid) {
   if(row==64 || column==64){p->valid=false;break;}
   auto* cell=expression(p,size,false,true);if(!cell)break;
   cell->ax=float(column);cell->ay=float(row);append(grid,cell);
   widths[column]=std::fmax(widths[column],cell->width);ascents[row]=std::fmax(ascents[row],cell->ascent);descents[row]=std::fmax(descents[row],cell->descent);
   if(column+1>columns)columns=column+1;
   if(p->pos<source.length && source.chars[p->pos]=='&'){++p->pos;++column;continue;}
   if(p->pos+1<source.length && source.chars[p->pos]=='\\' && source.chars[p->pos+1]=='\\'){p->pos+=2;++row;column=0;continue;}
   if(p->pos+5<=source.length && !std::memcmp(source.chars+p->pos,"\\end{",5)) {
    p->pos+=5;
    if(p->pos+env_len>=source.length || std::memcmp(source.chars+p->pos,source.chars+env,env_len) || source.chars[p->pos+env_len]!='}'){p->valid=false;break;}
    p->pos+=env_len+1;break;
   }
   p->valid=false;break;
  }
  if(!p->valid)return nullptr;
  float x[64]={},y[64]={};
  for(uint32_t cidx=1;cidx<columns;++cidx)x[cidx]=x[cidx-1]+widths[cidx-1]+size*.7f;
  for(uint32_t ridx=1;ridx<=row;++ridx)y[ridx]=y[ridx-1]+descents[ridx-1]+size*.3f+ascents[ridx];
  const float total=ascents[0]+y[row]+descents[row],baseline=total*.5f;
  for(auto* cell=grid->first;cell;cell=cell->next){const uint32_t cidx=uint32_t(cell->ax),ridx=uint32_t(cell->ay);cell->ax=x[cidx]+(widths[cidx]-cell->width)*.5f;cell->ay=y[ridx]+ascents[0]-baseline;}
  grid->width=columns ? x[columns-1]+widths[columns-1] : 0;grid->ascent=baseline;grid->descent=total-baseline;
  if(delimiter){grid->text=delimiter=='p' ? ETCH_STR_LIT("()") : ETCH_STR_LIT("[]");grid->width+=size*.7f;for(auto* cell=grid->first;cell;cell=cell->next)cell->ax+=size*.35f;}
  return grid;
 }
 if(is("frac") || is("dfrac") || is("tfrac")) {
  auto* box=make(p,Fraction,size);if(!box)return nullptr;
  box->a=atom(p,size*.8f);box->b=atom(p,size*.8f);if(!box->a || !box->b)return nullptr;
  box->width=std::fmax(box->a->width,box->b->width)+size*.4f;
  box->ax=(box->width-box->a->width)*.5f;box->bx=(box->width-box->b->width)*.5f;
  box->ay=-size*.35f-box->a->descent;box->by=size*.15f+box->b->ascent;
  box->ascent=box->a->ascent-box->ay;box->descent=box->by+box->b->descent;return box;
 }
 if(is("sqrt")) {
  auto* box=make(p,Root,size);if(!box)return nullptr;box->a=atom(p,size);if(!box->a)return nullptr;
  box->ax=size*.6f;box->width=box->ax+box->a->width+size*.1f;
  box->ascent=box->a->ascent+size*.15f;box->descent=box->a->descent;return box;
 }
 if(is("hat") || is("vec") || is("overline")) {
  auto* box=make(p,Accent,size);if(!box)return nullptr;box->a=atom(p,size);if(!box->a)return nullptr;
  box->text=is("hat") ? ETCH_STR_LIT("^") : is("vec") ? ETCH_STR_LIT("→") : EtchStr{};
  box->width=box->a->width;box->ascent=box->a->ascent+size*.3f;box->descent=box->a->descent;return box;
 }
 if(is("left")) {
  auto* open=atom(p,size);if(!open || open->kind!=Glyph || open->text.length!=1){p->valid=false;return nullptr;}
  auto* content=expression(p,size,false,false,true);if(!content || !p->valid)return nullptr;
  auto* close=atom(p,size);if(!close || close->kind!=Glyph || close->text.length!=1){p->valid=false;return nullptr;}
  auto* box=make(p,Delimited,size);if(!box)return nullptr;
  auto* delimiters=static_cast<char*>(etch_frame_arena_alloc(etch_current_context(),2,1));if(!delimiters){p->valid=false;return nullptr;}
  delimiters[0]=open->text.chars[0];delimiters[1]=close->text.chars[0];
  if(!std::strchr("([.{",delimiters[0]) || !std::strchr(")].}",delimiters[1])){p->valid=false;return nullptr;}
  box->text={delimiters,2,false};box->a=content;box->ax=size*.35f;box->width=content->width+size*.7f;box->ascent=content->ascent;box->descent=content->descent;return box;
 }
 if(is("right")){p->valid=false;return nullptr;}
 struct Symbol {const char* name;EtchStr text;};
 static const Symbol symbols[]={
  {"alpha",ETCH_STR_LIT("α")},{"beta",ETCH_STR_LIT("β")},{"gamma",ETCH_STR_LIT("γ")},{"delta",ETCH_STR_LIT("δ")},
  {"epsilon",ETCH_STR_LIT("ε")},{"zeta",ETCH_STR_LIT("ζ")},{"eta",ETCH_STR_LIT("η")},{"theta",ETCH_STR_LIT("θ")},
  {"iota",ETCH_STR_LIT("ι")},{"kappa",ETCH_STR_LIT("κ")},{"lambda",ETCH_STR_LIT("λ")},{"mu",ETCH_STR_LIT("μ")},
  {"nu",ETCH_STR_LIT("ν")},{"xi",ETCH_STR_LIT("ξ")},{"omicron",ETCH_STR_LIT("ο")},{"pi",ETCH_STR_LIT("π")},
  {"rho",ETCH_STR_LIT("ρ")},{"sigma",ETCH_STR_LIT("σ")},{"tau",ETCH_STR_LIT("τ")},{"upsilon",ETCH_STR_LIT("υ")},
  {"phi",ETCH_STR_LIT("φ")},{"chi",ETCH_STR_LIT("χ")},{"psi",ETCH_STR_LIT("ψ")},{"omega",ETCH_STR_LIT("ω")},
  {"Gamma",ETCH_STR_LIT("Γ")},{"Delta",ETCH_STR_LIT("Δ")},{"Theta",ETCH_STR_LIT("Θ")},{"Lambda",ETCH_STR_LIT("Λ")},
  {"Xi",ETCH_STR_LIT("Ξ")},{"Pi",ETCH_STR_LIT("Π")},{"Sigma",ETCH_STR_LIT("Σ")},{"Upsilon",ETCH_STR_LIT("Υ")},
  {"Phi",ETCH_STR_LIT("Φ")},{"Psi",ETCH_STR_LIT("Ψ")},{"Omega",ETCH_STR_LIT("Ω")},
  {"varepsilon",ETCH_STR_LIT("ϵ")},{"vartheta",ETCH_STR_LIT("ϑ")},{"varpi",ETCH_STR_LIT("ϖ")},{"varrho",ETCH_STR_LIT("ϱ")},{"varsigma",ETCH_STR_LIT("ς")},{"varphi",ETCH_STR_LIT("ϕ")},
  {"mp",ETCH_STR_LIT("∓")},{"div",ETCH_STR_LIT("÷")},{"ast",ETCH_STR_LIT("∗")},{"star",ETCH_STR_LIT("⋆")},{"circ",ETCH_STR_LIT("∘")},{"bullet",ETCH_STR_LIT("•")},
  {"oplus",ETCH_STR_LIT("⊕")},{"otimes",ETCH_STR_LIT("⊗")},{"wedge",ETCH_STR_LIT("∧")},{"vee",ETCH_STR_LIT("∨")},{"neg",ETCH_STR_LIT("¬")},{"forall",ETCH_STR_LIT("∀")},{"exists",ETCH_STR_LIT("∃")},
  {"emptyset",ETCH_STR_LIT("∅")},{"supset",ETCH_STR_LIT("⊃")},{"supseteq",ETCH_STR_LIT("⊇")},{"to",ETCH_STR_LIT("→")},{"mapsto",ETCH_STR_LIT("↦")},{"leftrightarrow",ETCH_STR_LIT("↔")},{"Leftrightarrow",ETCH_STR_LIT("⇔")},
  {"ldots",ETCH_STR_LIT("…")},{"cdots",ETCH_STR_LIT("⋯")},{"vdots",ETCH_STR_LIT("⋮")},{"ddots",ETCH_STR_LIT("⋱")},{"perp",ETCH_STR_LIT("⊥")},{"parallel",ETCH_STR_LIT("∥")},{"angle",ETCH_STR_LIT("∠")},
  {"le",ETCH_STR_LIT("≤")},{"ge",ETCH_STR_LIT("≥")},{"ne",ETCH_STR_LIT("≠")},{"simeq",ETCH_STR_LIT("≃")},{"sim",ETCH_STR_LIT("∼")},{"propto",ETCH_STR_LIT("∝")},{"ll",ETCH_STR_LIT("≪")},{"gg",ETCH_STR_LIT("≫")},
  {"oint",ETCH_STR_LIT("∮")},{"coprod",ETCH_STR_LIT("∐")},{"bigcup",ETCH_STR_LIT("⋃")},{"bigcap",ETCH_STR_LIT("⋂")},
  {"arcsin",ETCH_STR_LIT("arcsin")},{"arccos",ETCH_STR_LIT("arccos")},{"arctan",ETCH_STR_LIT("arctan")},{"sinh",ETCH_STR_LIT("sinh")},{"cosh",ETCH_STR_LIT("cosh")},{"tanh",ETCH_STR_LIT("tanh")},{"min",ETCH_STR_LIT("min")},{"max",ETCH_STR_LIT("max")},{"det",ETCH_STR_LIT("det")},
  {"sum",ETCH_STR_LIT("∑")},{"prod",ETCH_STR_LIT("∏")},{"int",ETCH_STR_LIT("∫")},{"infty",ETCH_STR_LIT("∞")},
  {"times",ETCH_STR_LIT("×")},{"cdot",ETCH_STR_LIT("·")},{"pm",ETCH_STR_LIT("±")},{"leq",ETCH_STR_LIT("≤")},
  {"geq",ETCH_STR_LIT("≥")},{"neq",ETCH_STR_LIT("≠")},{"approx",ETCH_STR_LIT("≈")},{"equiv",ETCH_STR_LIT("≡")},
  {"partial",ETCH_STR_LIT("∂")},{"nabla",ETCH_STR_LIT("∇")},{"in",ETCH_STR_LIT("∈")},{"notin",ETCH_STR_LIT("∉")},
  {"subset",ETCH_STR_LIT("⊂")},{"subseteq",ETCH_STR_LIT("⊆")},{"cup",ETCH_STR_LIT("∪")},{"cap",ETCH_STR_LIT("∩")},
  {"rightarrow",ETCH_STR_LIT("→")},{"leftarrow",ETCH_STR_LIT("←")},{"Rightarrow",ETCH_STR_LIT("⇒")},
  {"sin",ETCH_STR_LIT("sin")},{"cos",ETCH_STR_LIT("cos")},{"tan",ETCH_STR_LIT("tan")},{"log",ETCH_STR_LIT("log")},
  {"ln",ETCH_STR_LIT("ln")},{"lim",ETCH_STR_LIT("lim")},{"exp",ETCH_STR_LIT("exp")},
 };
 for(const auto& symbol:symbols)if(is(symbol.name))return glyph(p,symbol.text,size);
 p->valid=false;return nullptr;
}
Box* atom(Parser* p,float size) {
 if(++p->depth>64){p->valid=false;--p->depth;return nullptr;}
 auto* box=atom_impl(p,size);--p->depth;return box;
}
Box* expression(Parser* p,float size,bool grouped,bool cell,bool delimited) {
 if(++p->depth>64){p->valid=false;--p->depth;return nullptr;}
 auto* row=make(p,Row,size);if(!row){--p->depth;return nullptr;}
 while(p->valid) {
  spaces(p);if(p->pos==p->source.length){if(grouped)p->valid=false;break;}
  if(delimited && p->pos+6<=p->source.length && !std::memcmp(p->source.chars+p->pos,"\\right",6)){p->pos+=6;break;}
  if(cell && (p->source.chars[p->pos]=='&' || (p->pos+1<p->source.length && p->source.chars[p->pos]=='\\' && p->source.chars[p->pos+1]=='\\') || (p->pos+5<=p->source.length && !std::memcmp(p->source.chars+p->pos,"\\end{",5))))break;
  if(p->source.chars[p->pos]=='}'){if(grouped)++p->pos;else p->valid=false;break;}
  auto* base=atom(p,size);if(!base)break;
  Box* scripts=nullptr;
  while(p->valid) {
   spaces(p);if(p->pos==p->source.length)break;const char c=p->source.chars[p->pos];if(c!='^' && c!='_')break;++p->pos;
   if(!scripts){scripts=make(p,Scripts,size);if(!scripts)break;scripts->a=base;}
   auto* value=atom(p,size*.7f);if(!value)break;
   if(c=='^'){if(scripts->b)p->valid=false;scripts->b=value;}else{if(scripts->c)p->valid=false;scripts->c=value;}
  }
  if(scripts) {
   scripts->width=base->width;scripts->ascent=base->ascent;scripts->descent=base->descent;
   float extra=0;
   if(scripts->b){scripts->bx=base->width; scripts->by=-std::fmax(size*.5f,base->ascent-size*.2f);extra=scripts->b->width;scripts->ascent=std::fmax(scripts->ascent,scripts->b->ascent-scripts->by);scripts->descent=std::fmax(scripts->descent,scripts->by+scripts->b->descent);}
   if(scripts->c){scripts->cx=base->width;scripts->cy=std::fmax(size*.25f,base->descent+scripts->c->ascent*.6f);extra=std::fmax(extra,scripts->c->width);scripts->descent=std::fmax(scripts->descent,scripts->cy+scripts->c->descent);scripts->ascent=std::fmax(scripts->ascent,scripts->c->ascent-scripts->cy);}
   scripts->width+=extra; base=scripts;
  }
  append(row,base);
 }
 --p->depth;return row;
}
void line(float x,float y,float w,float size,EtchColor color) {
 EtchLineDesc desc={};desc.from={x,y};desc.to={x+w,y};desc.thickness=std::fmax(1.f,size*.055f);desc.color=color;etch_draw_line(&desc);
}
void draw(const Box* box,float x,float y,EtchColor color) {
 if(box->kind==Glyph){auto desc=etch_text_draw_desc_default();desc.text=box->text;desc.pos={x,y-box->ascent};desc.size=box->size;desc.color=color;etch_draw_text(&desc);return;}
 if(box->kind==Grid || box->kind==Delimited){
  if(box->kind==Delimited)draw(box->a,x+box->ax,y,color);
  for(auto* child=box->first;child;child=child->next)draw(child,x+child->ax,y+child->ay,color);
  if(box->text.length) {
   for(uint32_t side=0;side<2;++side) {
    const float sx=x+(side ? box->width-box->size*.3f : 0),top=y-box->ascent,bottom=y+box->descent;
    const char mark=box->text.chars[side];
    if(mark=='.')continue;
    if(mark=='[' || mark==']') {
     EtchLineDesc d={};d.color=color;d.thickness=std::fmax(1.f,box->size*.055f);d.from={sx+(side ? 0 : box->size*.15f),top};d.to={sx+(side ? box->size*.15f : 0),top};etch_draw_line(&d);d.from=d.to;d.to.y=bottom;etch_draw_line(&d);d.from=d.to;d.to.x=sx+(side ? 0 : box->size*.15f);etch_draw_line(&d);
    } else if(mark=='{' || mark=='}') {
     const float edge=sx+(side ? 0 : box->size*.3f),stem=sx+box->size*.15f,tip=sx+(side ? box->size*.3f : 0),mid=(top+bottom)*.5f;
     auto d=etch_bezier_desc_default();d.cubic=false;d.thickness=std::fmax(1.f,box->size*.055f);d.color_start=color;d.color_end=color;
     d.p0={edge,top};d.p1={stem,top};d.p2={stem,(top+mid)*.5f};etch_draw_bezier(&d);
     d.p0=d.p2;d.p1={stem,mid};d.p2={tip,mid};etch_draw_bezier(&d);
     d.p0=d.p2;d.p1={stem,mid};d.p2={stem,(bottom+mid)*.5f};etch_draw_bezier(&d);
     d.p0=d.p2;d.p1={stem,bottom};d.p2={edge,bottom};etch_draw_bezier(&d);
    } else {
     auto d=etch_bezier_desc_default();d.p0={sx+box->size*.15f,top};d.p2={sx+box->size*.15f,bottom};d.p1={sx+(side ? box->size*.4f : -box->size*.1f),(top+bottom)*.5f};d.cubic=false;d.thickness=std::fmax(1.f,box->size*.055f);d.color_start=color;d.color_end=color;etch_draw_bezier(&d);
    }
   }
  }
  return;
 }
 if(box->kind==Row){for(auto* child=box->first;child;child=child->next){draw(child,x,y,color);x+=child->width;}return;}
 if(box->a)draw(box->a,x+box->ax,y+box->ay,color);
 if(box->b)draw(box->b,x+box->bx,y+box->by,color);
 if(box->c)draw(box->c,x+box->cx,y+box->cy,color);
 if(box->kind==Fraction)line(x,y-box->size*.2f,box->width,box->size,color);
 if(box->kind==Root){
  line(x+box->ax,y-box->ascent,box->width-box->ax,box->size,color);
  const float left=x,top=y-box->ascent,bottom=y+box->descent,thickness=std::fmax(1.f,box->size*.055f);
  EtchVec2 points[]={{left,y-box->size*.1f},{left+box->size*.17f,y-box->size*.22f},{left+box->size*.3f,bottom-thickness},{left+box->ax,top-thickness},{left+box->ax+thickness,top},{left+box->size*.3f,bottom+thickness},{left+box->size*.13f,y-box->size*.15f}};
  EtchFilledPolygonDesc radical={};radical.points=points;radical.point_count=7;radical.fill=color;etch_draw_filled_polygon(&radical);
 }
 if(box->kind==Accent) {
  if(!box->text.length)line(x,y-box->ascent+box->size*.1f,box->width,box->size,color);
  else {
   const float top=y-box->ascent+box->size*.05f;
   EtchLineDesc d={};d.thickness=std::fmax(1.f,box->size*.055f);d.color=color;
   if(box->text.chars[0]=='^'){d.from={x,top+box->size*.18f};d.to={x+box->width*.5f,top};etch_draw_line(&d);d.from=d.to;d.to={x+box->width,top+box->size*.18f};etch_draw_line(&d);}
   else {d.from={x,top+box->size*.09f};d.to={x+box->width,top+box->size*.09f};etch_draw_line(&d);d.from={d.to.x-box->size*.18f,top};etch_draw_line(&d);d.from.y=top+box->size*.18f;etch_draw_line(&d);}
  }
 }
}
InlayMathResult layout(EtchStr tex,float size,const InlayMathDesc* desc) {
 auto* context=etch_current_context();
 if(!context || !etch_frame_in_progress(context) || (!tex.chars && tex.length) || tex.length>65536 || !std::isfinite(size) || size<6 || size>256)return {};
 Parser parser={tex,0,true,0};const auto* box=expression(&parser,size,false);if(!box || !parser.valid)return {};
 if(desc)draw(box,desc->pos.x,desc->pos.y+box->ascent,desc->color);
 return {box->width,box->ascent+box->descent,box->ascent,true};
}
}
extern "C" {
InlayMathDesc inlay_math_desc_default(void){return {{0,0},20,{230,235,245,255}};}
InlayMathResult inlay_measure_math(EtchStr tex,float size){return layout(tex,size,nullptr);}
InlayMathResult inlay_draw_math(EtchStr tex,const InlayMathDesc* desc){if(!desc || !std::isfinite(desc->pos.x) || !std::isfinite(desc->pos.y))return {};return layout(tex,desc->font_size_px,desc);}
}
