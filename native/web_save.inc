/* Reject unsafe player state before it enters the original ARM routines.
   The firmware reads these packed fields as 32-bit little-endian integers. */
static u32 web_save_word(const u8 *p,u32 off){
 return (u32)p[off]|((u32)p[off+1]<<8)|((u32)p[off+2]<<16)|((u32)p[off+3]<<24);
}
static void web_restore_item_limits(u8 *p){
 if(web_save_word(p,0x41u)>3u){p[0x41]=3;p[0x42]=p[0x43]=p[0x44]=0;}
}
static int web_valid_save(const u8 *p){
 u32 floor=web_save_word(p,5),visited=web_save_word(p,9);
 if(floor>50u||visited<1u||visited>50u||
    web_save_word(p,0x76)>10u||web_save_word(p,0x7a)>10u||
    web_save_word(p,0x7e)>10u||web_save_word(p,0x82)>10u)return 0;
 if(web_save_word(p,0x1d)!=1u||web_save_word(p,0x21)>1u||!web_save_word(p,0x46))return 0;
 static const u32 stats[]={0x46,0x4a,0x4e,0x52,0x5a,0x5e,0x62};
 for(u32 i=0;i<sizeof(stats)/sizeof(stats[0]);i++)if(web_save_word(p,stats[i])>0x7fffffffu)return 0;
 return 1;
}
