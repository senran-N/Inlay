#include "inlay_internal.h"
#include <cstdlib>
#include <cstring>
#include <cmath>


namespace {
constexpr uint32_t max_bytes=64u*1024u*1024u;
bool reserve_source(InlayDoc* d,uint32_t needed) noexcept {
 if(needed<=d->source_capacity)return true;
 uint32_t cap=d->source_capacity;
 while(cap<needed)cap=cap<max_bytes/2 ? cap*2 : max_bytes;
 void* data=std::realloc(d->source,cap);
 if(!data)return false;
 d->source=static_cast<char*>(data);d->source_capacity=cap;return true;
}
bool reserve_blocks(InlayDoc* d,uint32_t needed) noexcept {
 if(needed<=d->block_capacity)return true;
 uint32_t cap=d->block_capacity;
 while(cap<needed)cap*=2;
 void* data=std::realloc(d->blocks,size_t(cap)*sizeof(InlayBlock));
 if(!data)return false;
 d->blocks=static_cast<InlayBlock*>(data);d->block_capacity=cap;return true;
}
bool starts(const char* text,uint32_t length,const char* prefix) noexcept {
 const size_t n=std::strlen(prefix);return n<=length && std::memcmp(text,prefix,n)==0;
}
InlayBlock classify(const char* text,uint32_t start,uint32_t length) noexcept {
 InlayBlock b={};b.kind=INLAY_PARAGRAPH;b.offset=start;b.length=length;b.content_offset=start;b.content_length=length;
 uint32_t indent=0;while(indent<length && indent<3 && text[indent]==' ')++indent;
 const char* s=text+indent;const uint32_t n=length-indent;
 uint32_t h=0;while(h<n && s[h]=='#')++h;
 if(h>0 && h<=6 && h<n && s[h]==' ') {b.kind=INLAY_HEADING;b.level=h;b.content_offset=start+indent+h+1;}
 else if(starts(s,n,"```") || starts(s,n,"~~~")) {
  b.kind=INLAY_CODE_BLOCK;b.level=0;
  while(b.level<n && s[b.level]==s[0])++b.level;
  b.language_offset=start+indent+b.level;b.language_length=n-b.level;b.content_offset=start+length;
 }
 else if(starts(s,n,"> [!")) {b.kind=INLAY_ALERT;b.content_offset=start+indent+2;}
 else if(starts(s,n,"> ")) {b.kind=INLAY_BLOCKQUOTE;b.content_offset=start+indent+2;}
 else if(starts(s,n,"<details")) {b.kind=INLAY_DETAILS;}
 else if(starts(s,n,"- [ ] ") || starts(s,n,"- [x] ") || starts(s,n,"- [X] ")) {b.kind=INLAY_TASK_LIST;}
 else if(starts(s,n,"- ") || starts(s,n,"* ") || starts(s,n,"+ "))b.kind=INLAY_LIST;
 else {
  uint32_t digits=0;while(digits<n && s[digits]>='0' && s[digits]<='9')++digits;
  if(digits && digits+1<n && (s[digits]=='.' || s[digits]==')') && s[digits+1]==' ')b.kind=INLAY_LIST;
 }
 if(n>=3) {
  uint32_t marks=0;const char mark=s[0];bool thematic=mark=='-' || mark=='*' || mark=='_';
  for(uint32_t i=0;i<n;++i) {if(s[i]==mark)++marks;else if(s[i]!=' ' && s[i]!='\t')thematic=false;}
  if(thematic && marks>=3)b.kind=INLAY_THEMATIC_BREAK;
 }
 b.content_length=start+length-b.content_offset;return b;
}
bool table_rule(const char* s,uint32_t n) noexcept {
 uint32_t dashes=0;bool pipe=false;
 for(uint32_t i=0;i<n;++i) {
  if(s[i]=='-')++dashes;else if(s[i]=='|')pipe=true;
  else if(s[i]!=':' && s[i]!=' ' && s[i]!='\t')return false;
 }
 return pipe && dashes>=3;
}
void parse_tail(InlayDoc* d) noexcept {
 d->block_count=d->frozen_count;
 uint32_t pos=d->tail_start;InlayBlock* current=nullptr;
 while(pos<d->source_size) {
  uint32_t end=pos;while(end<d->source_size && d->source[end]!='\n')++end;
  const bool full=end<d->source_size;uint32_t length=end-pos;
  if(length && d->source[end-1]=='\r')--length;
  const char* line=d->source+pos;
  const uint32_t after=full ? end+1 : end;
  bool blank=true;for(uint32_t i=0;i<length;++i)if(line[i]!=' ' && line[i]!='\t')blank=false;
  if(current && current->kind==INLAY_CODE_BLOCK) {
   uint32_t marker_pos=current->offset;while(d->source[marker_pos]==' ')++marker_pos;
   const char marker=d->source[marker_pos];uint32_t indent=0;while(indent<length && indent<3 && line[indent]==' ')++indent;
   uint32_t marks=indent;while(marks<length && line[marks]==marker)++marks;
   bool closing=marks-indent>=current->level;
   for(uint32_t i=marks;i<length;++i)if(line[i]!=' ' && line[i]!='\t')closing=false;
   if(closing) {
    current->length=after-current->offset;current->content_length=pos-current->content_offset;
    if(full) {current->complete=true;d->frozen_count=d->block_count;d->tail_start=after;current=nullptr;}
   } else {current->length=after-current->offset;current->content_length=after-current->content_offset;}
  } else if(current && current->kind==INLAY_DETAILS) {
   current->length=after-current->offset;current->content_length=current->length;
   if(starts(line,length,"</details>") && full) {
    current->complete=true;d->frozen_count=d->block_count;d->tail_start=after;current=nullptr;
   }
  } else if(blank && full) {
   if(current)current->complete=true;
   d->frozen_count=d->block_count;d->tail_start=after;current=nullptr;
  } else if(!blank) {
   InlayBlock b=classify(line,pos,length);
   if(current && current->kind==INLAY_PARAGRAPH && table_rule(line,length))current->kind=INLAY_TABLE;
   const bool continuing=current && (current->kind==b.kind || (current->kind==INLAY_ALERT && b.kind==INLAY_BLOCKQUOTE) || (current->kind==INLAY_TABLE && std::memchr(line,'|',length)));
   if(continuing) {
    current->length=after-current->offset;current->content_length=after-current->content_offset;
   } else {
    if(current && full) {current->complete=true;d->frozen_count=d->block_count;d->tail_start=pos;}
    d->blocks[d->block_count++]=b;current=&d->blocks[d->block_count-1];
    current->length=after-pos;
    if(b.kind==INLAY_CODE_BLOCK && full) {current->content_offset=after;current->content_length=0;}
    if(full && (b.kind==INLAY_HEADING || b.kind==INLAY_THEMATIC_BREAK)) {
     current->complete=true;d->frozen_count=d->block_count;d->tail_start=after;current=nullptr;
    }
   }
  }
  pos=after;
 }
}
}
extern "C" {
InlayDocConfig inlay_doc_config_default(void) {return {4096,64};}
InlayDocViewDesc inlay_doc_view_desc_default(void) {return {0,640,480,15};}
InlayDoc* inlay_doc_create(EtchContext* context,const InlayDocConfig* config) {
 if(!context)return nullptr;
 const InlayDocConfig c=config ? *config : inlay_doc_config_default();
 if(c.initial_source_bytes<4 || c.initial_source_bytes>max_bytes || !c.initial_blocks || c.initial_blocks>max_bytes)return nullptr;
 auto* d=static_cast<InlayDoc*>(std::calloc(1,sizeof(InlayDoc)));if(!d)return nullptr;
 d->context=context;d->source_capacity=c.initial_source_bytes;d->block_capacity=c.initial_blocks;
 d->source=static_cast<char*>(std::malloc(d->source_capacity));d->blocks=static_cast<InlayBlock*>(std::malloc(size_t(d->block_capacity)*sizeof(InlayBlock)));
 if(!d->source || !d->blocks) {inlay_doc_destroy(d);return nullptr;}return d;
}
void inlay_doc_destroy(InlayDoc* d) {if(d) {for(auto& view:d->views) {std::free(view.rows);std::free(view.heights);std::free(view.open);std::free(view.details);std::free(view.tokens);}std::free(d->source);std::free(d->blocks);std::free(d);}}
void inlay_doc_clear(InlayDoc* d) {if(d) {d->source_size=0;d->block_count=0;d->frozen_count=0;d->tail_start=0;d->utf8_carry_len=0;for(auto& view:d->views)view.width=0;++d->revision;etch_request_redraw(d->context);}}
bool inlay_doc_append_utf8(InlayDoc* d,EtchStr chunk) {
 if(!d || (!chunk.chars && chunk.length) || chunk.length>max_bytes-d->source_size-d->utf8_carry_len)return false;
 const uintptr_t ptr=reinterpret_cast<uintptr_t>(chunk.chars),base=reinterpret_cast<uintptr_t>(d->source);
 const bool aliases=ptr>=base && ptr-base<d->source_size;
 const uint32_t source_offset=aliases ? uint32_t(ptr-base) : 0;
 if(aliases && chunk.length>d->source_size-source_offset)return false;
 const uint32_t total=d->utf8_carry_len+chunk.length;
 const auto byte=[&](uint32_t i) noexcept {return uint8_t(i<d->utf8_carry_len ? d->utf8_carry[i] : chunk.chars[i-d->utf8_carry_len]);};
 uint32_t valid=0,newlines=0;
 while(valid<total) {
  const uint8_t lead=byte(valid);uint32_t n=1,cp=lead;
  if(lead>=0xc2 && lead<=0xdf) {n=2;cp=lead&31;}
  else if(lead>=0xe0 && lead<=0xef) {n=3;cp=lead&15;}
  else if(lead>=0xf0 && lead<=0xf4) {n=4;cp=lead&7;}
  else if(lead>=0x80 || lead==0)return false;
  for(uint32_t j=1;j<n && valid+j<total;++j)if((byte(valid+j)&0xc0)!=0x80)return false;
  if(total-valid<n)break;
  for(uint32_t j=1;j<n;++j)cp=(cp<<6)|(byte(valid+j)&63);
  if((n==3 && cp<0x800) || (n==4 && cp<0x10000) || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))return false;
  if(lead=='\n')++newlines;
  valid+=n;
 }
 if(!reserve_source(d,d->source_size+valid) || !reserve_blocks(d,d->block_count+newlines+2))return false;
 if(aliases)chunk.chars=d->source+source_offset;
 char carry[4]={};const uint32_t carry_count=total-valid;
 for(uint32_t i=0;i<carry_count;++i)carry[i]=char(byte(valid+i));
 for(uint32_t i=0;i<valid;++i)d->source[d->source_size+i]=char(byte(i));
 d->source_size+=valid;d->utf8_carry_len=uint8_t(carry_count);std::memcpy(d->utf8_carry,carry,4);
 parse_tail(d);++d->revision;etch_request_redraw(d->context);return true;
}
bool inlay_doc_set_utf8(InlayDoc* d,EtchStr text) {if(!d)return false;inlay_doc_clear(d);return inlay_doc_append_utf8(d,text);}
uint32_t inlay_doc_block_count(const InlayDoc* d) {return d ? d->block_count : 0;}
const InlayBlock* inlay_doc_block(const InlayDoc* d,uint32_t i) {return d && i<d->block_count ? d->blocks+i : nullptr;}
EtchStr inlay_doc_source(const InlayDoc* d) {return d ? EtchStr{d->source,d->source_size,false} : EtchStr{};}
}
