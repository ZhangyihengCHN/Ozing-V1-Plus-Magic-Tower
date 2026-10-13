/* Pixel fonts at the original V1PRU resource boundaries, with native advances. */
extern const u8 _binary_font_ascii12_bin_start[], _binary_font_ascii12_bin_end[];
extern const u8 _binary_font_cjk12_bin_start[], _binary_font_cjk12_bin_end[];

static int web_text_height(Obj *dc){
 Obj *font=dc?getobj(dc->selected_font):0;
 return font&&font->kind==OBJ_FONT&&font->height<=12?12:16;
}
static const u8 *web_ascii_glyph(u8 ch,int height,int *width){
 const u8 *base=height==12?_binary_font_ascii12_bin_start:_binary_font96_bin_start;
 u32 len=(u32)((height==12?_binary_font_ascii12_bin_end:_binary_font96_bin_end)-base);
 u32 e=(ch&127u)*5u;
 if(e+5u>len)return 0;
 *width=base[e];
 u32 off=(u32)base[e+1]|((u32)base[e+2]<<8)|((u32)base[e+3]<<16)|((u32)base[e+4]<<24);
 return off+(u32)height<=len?base+off:0;
}
static const u8 *web_cjk_glyph(u8 lead,u8 trail,int height){
 i32 idx=(i32)trail+(i32)lead*208-0x6900;
 if(idx<0)return 0;
 const u8 *base=height==12?_binary_font_cjk12_bin_start:_binary_font100_bin_start;
 u32 len=(u32)((height==12?_binary_font_cjk12_bin_end:_binary_font100_bin_end)-base);
 u32 size=(u32)height*2u,off=(u32)idx*size;
 return off+size<=len?base+off:0;
}
static int web_text_width(Obj *dc,const u8 *s,u32 avail){
 int height=web_text_height(dc),width=height/2;
 if(s[0]>=0x81&&avail>=2)return height;
 web_ascii_glyph(s[0],height,&width);
 return width;
}
static int draw_glyph(Obj *dc,int x,int y,const u8 *s,u32 avail){
 if(!dc||!dc->framebuffer||!s||!avail)return 0;
 int height=web_text_height(dc),cjk=s[0]>=0x81&&avail>=2,width=height/2;
 const u8 *glyph=cjk?web_cjk_glyph(s[0],s[1],height):web_ascii_glyph(s[0],height,&width);
 if(cjk)width=height;
 if(!glyph)return width;
 u32 fg=cref_to_rgb(dc->text_color),bg=cref_to_rgb(dc->bk_color);
 for(int yy=0;yy<height;yy++){
  u32 bits=cjk?((u32)glyph[yy*2]<<8)|glyph[yy*2+1]:glyph[yy];
  for(int xx=0;xx<width;xx++){
   if(bits&(1u<<((cjk?15:7)-xx)))putpx(x+dc->origin_x+xx,y+dc->origin_y+yy,fg);
   else if(dc->bk_mode==2)putpx(x+dc->origin_x+xx,y+dc->origin_y+yy,bg);
  }
 }
 return width;
}
static void draw_text_bytes(Obj *dc,int x,int y,const u8 *s,u32 n,int maxw,int wrap){
 int cx=x,cy=y,height=web_text_height(dc);u32 i=0;
 while(i<n&&s[i]){
  if(s[i]=='\r'){i++;continue;}
  if(s[i]=='\n'){cx=x;cy+=height;i++;continue;}
  u32 take=s[i]>=0x81&&i+1<n?2:1;
  int advance=web_text_width(dc,s+i,n-i);
  if(wrap&&maxw>0&&cx-x+advance>maxw){cx=x;cy+=height;}
  draw_glyph(dc,cx,cy,s+i,n-i);cx+=advance;i+=take;
 }
}
