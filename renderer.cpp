#define GLEW_STATIC
#include "renderer.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstring>

int WinW=1280,WinH=1280;Camera g_cam;GLFWwindow* g_window=0;
bool g_texDirty=true,g_needRender=true;
std::string g_overlayText;float g_wireAlpha=0.5f;
float g_textClr[4]={1,1,1,1};float g_sunClr[3]={0,0,0};
float g_wallClr[3]={0.5f,0.5f,0.5f};float g_bgClr[3]={0.267f,0.267f,0.267f};
int g_circleR=64;float g_noise=0.0f;int g_batchN=3;float g_emit=1.0f;

static GLuint cvsColorTex=0,scanTex=0,renderTex=0,avgTex=0,occTex=0,lvlTex=0,accTex[2]={0,0},scanSSBO=0,dirSSBO=0;
static GLuint clearProg=0,scanProg=0,blendProg=0,mipAvProg=0,mipOccProg=0,occInitProg=0,lvlProg=0,rawProg=0,mipProg=0,accProg=0,ditherProg=0,copySSBOProg=0,fullVAO=0;
static int CW=0,CH=0,mipLevels=0;
bool g_useMip=false,g_useAcc=false;

// Direction table (rebuilt only when g_circleR changes) + cached uniform locations for the hot path.
static std::vector<GLint> g_dirs; static int g_dirsR=-1;
static struct{GLint cc,cs,amb,ndirs,cp,emit;}uScan={-1,-1,-1,-1,-1,-1};
static struct{GLint cs;}uCopy={-1};
static struct{GLint cc,scan,cs,emit;}uBlend={-1,-1,-1,-1};
static struct{GLint src,res,alpha,frame;}uDither={-1,-1,-1,-1};

// Dirty rectangle of the canvas texture, filled in by main.cpp's syncCanvas().
// g_texAll=true forces a full upload (first frame, grid load, colour change).
bool g_texAll=true;
int g_texX0=0,g_texY0=0,g_texX1=-1,g_texY1=-1;

void markCanvasFullDirty(){g_texAll=true;g_texDirty=true;}

static const char* fullFrag=R"(#version 330 core
uniform sampler2D u_t;uniform vec2 u_cp;uniform vec2 u_uv;
in vec2 vU;out vec4 oC;
void main(){
  vec2 ab=u_uv*(vU-0.5)+u_cp;
  oC=texture(u_t,ab);
})";

static const char* mipFrag=R"(#version 330 core
uniform sampler2D u_avg,u_lvl;uniform vec2 u_cp;uniform vec2 u_uv;uniform vec2 u_cv;
in vec2 vU;out vec4 oC;
void main(){
  vec2 ab=u_uv*(vU-0.5)+u_cp;
  ivec2 p=clamp(ivec2(ab*textureSize(u_lvl,0)),ivec2(0),textureSize(u_lvl,0)-1);
  int l=int(texelFetch(u_lvl,p,0).r*255.0+0.5);
  ivec2 sz=textureSize(u_avg,l);
  ivec2 bp=clamp(p/(1<<l),ivec2(0),sz-1);
  oC=vec4(clamp(texelFetch(u_avg,bp,l).rgb,0.0,1.0),1.0);
})";

static const char* vertSrc=R"(#version 330 core
layout(location=0)in vec2 aP;layout(location=1)in vec2 aU;out vec2 vU;
void main(){gl_Position=vec4(aP,0,1);vU=aU;})";

static const char* clearComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;layout(rgba16f,binding=0)uniform image2D u_img;uniform ivec2 u_cs;
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x<u_cs.x&&p.y<u_cs.y)imageStore(u_img,p,vec4(0));})";

static const char* scanComp=R"(#version 430 core
layout(local_size_x=256)in;uniform sampler2D u_cc;uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit;
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
void main(){
  int ri=int(gl_GlobalInvocationID.x);
  ivec2 ud=dirs[gl_GlobalInvocationID.z];
  int dx=abs(ud.x),dy=abs(ud.y);
  int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1,W=u_cs.x,H=u_cs.y;
  int rc=(dx>=dy)?(2*H+1):(2*W+1);
  // Keep the exact thread set of the old per-direction dispatch (ceil(rc/256)*256): those
  // padding rays are not inert - some start outside the grid and march into it, and they
  // contribute real light. Matching it keeps the rendered image byte-identical.
  if(ri>=((rc+255)/256)*256)return;
  int x,y;if(dx>=dy){x=(sx>0)?0:W-1;y=-H+ri;}else{x=-W+ri;y=(sy>0)?0:H-1;}
  vec3 lt=u_ambient;float invN=1.0/float(u_ndirs);int er=dx-dy;
  while(true){
    if(x>=0&&x<W&&y>=0&&y<H){
      vec4 c=texelFetch(u_cc,ivec2(x,y),0);
      vec3 em=c.rgb*c.a*u_emit;
      lt=lt*(1.0-c.a)+em;lt+=em*(1.0-c.a)*u_cp;
      int bi=(y*W+x)*3;
      vec3 add=lt*invN;
      atomicAdd(d[bi],uint(clamp(add.r,0.0,1.0)*65535.0));
      atomicAdd(d[bi+1],uint(clamp(add.g,0.0,1.0)*65535.0));
      atomicAdd(d[bi+2],uint(clamp(add.b,0.0,1.0)*65535.0));
    }
    int e2=2*er;if(e2>-dy){er-=dy;x+=sx;}if(e2<dx){er+=dx;y+=sy;}
    if((sx>0&&x>=W)||(sx<0&&x<0)||(sy>0&&y>=H)||(sy<0&&y<0))break;
  }
})";

static const char* blendComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;uniform sampler2D u_cc,u_scan;uniform ivec2 u_cs;
layout(rgba8,binding=0)writeonly uniform image2D u_out;
uniform float u_emit;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  vec4 c=texelFetch(u_cc,p,0);if(c.a>=1.0){imageStore(u_out,p,c);return;}
  vec3 s=texelFetch(u_scan,p,0).rgb;vec3 e=c.rgb*c.a*u_emit;vec3 r=mix(s,e,c.a)+e;float pk=max(max(r.r,r.g),r.b);if(pk>1.0)r/=pk;
  imageStore(u_out,p,vec4(r,1.0));
})";

static const char* mipAvComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;uniform sampler2D u_src;uniform ivec2 u_sz;uniform int u_lod;
layout(rgba8,binding=0)writeonly uniform image2D u_avg;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);vec4 mx=vec4(0);
  for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++){
    ivec2 sp=ivec2(p.x*2+dx,p.y*2+dy);
    if(sp.x<u_sz.x&&sp.y<u_sz.y)mx=max(mx,texelFetch(u_src,sp,u_lod));
  }
  imageStore(u_avg,p,mx);
})";

static const char* mipOccComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;uniform sampler2D u_src;uniform ivec2 u_sz;uniform int u_lod;
layout(r8,binding=0)writeonly uniform image2D u_dst;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);float o=0;
  for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++){
    ivec2 sp=ivec2(p.x*2+dx,p.y*2+dy);
    if(sp.x<u_sz.x&&sp.y<u_sz.y&&texelFetch(u_src,sp,u_lod).r>0.5)o=1.0;
  }
  imageStore(u_dst,p,vec4(o));
})";

static const char* occInitComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;uniform sampler2D u_scene;uniform ivec2 u_cs;
layout(r8,binding=0)writeonly uniform image2D u_occ;
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x<u_cs.x&&p.y<u_cs.y)imageStore(u_occ,p,vec4(texelFetch(u_scene,p,0).a>0.01?1.0:0.0));})";

// Level compute: distance-field from occ mip -> display mip level
static const char* lvlComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_occ;uniform ivec2 u_res;uniform int u_mips;
layout(r8,binding=0)writeonly uniform image2D u_lvl;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_res.x||p.y>=u_res.y)return;
  int dlvl=u_mips;
  for(int l=0;l<u_mips;l++)if(texelFetch(u_occ,p/(1<<l),l).r>0.5){dlvl=l;break;}
  int lvl;
  if(dlvl==0)lvl=0;
  else if(dlvl>=u_mips)lvl=u_mips-1;
  else lvl=dlvl;
  imageStore(u_lvl,p,vec4(lvl/255.0));
})";

// Temporal EMA: acc_out = a*new + (1-a)*old
static const char* accComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_new,u_old;uniform float u_a;
layout(rgba16f,binding=0)uniform image2D u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  ivec2 sz=imageSize(u_out);
  if(p.x>=sz.x||p.y>=sz.y)return;
  vec4 n=texelFetch(u_new,p,0);
  vec4 o=texelFetch(u_old,p,0);
  imageStore(u_out,p,mix(o,n,u_a));
})";

// Dither: Interleaved Gradient Noise (COD:AW) adaptive dither
static const char* ditherComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_src;uniform ivec2 u_res;uniform float u_alpha;uniform int u_frame;
layout(rgba8,binding=0)writeonly uniform image2D u_dst;
float ign(vec2 p){return fract(52.9829189*fract(dot(p,vec2(0.06711056,0.00583715))));}
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_res.x||p.y>=u_res.y)return;
  vec3 s=texelFetch(u_src,p,0).rgb;
  vec3 b=vec3(0);for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)b+=texelFetch(u_src,clamp(p+ivec2(dx,dy),ivec2(0),u_res-1),0).rgb;
  float bm=max(max(abs(s.r-b.r/9.0),abs(s.g-b.g/9.0)),abs(s.b-b.b/9.0));
  float n=ign(vec2(p)+vec2(u_frame*137));
  imageStore(u_dst,p,vec4(clamp(s+(n-0.5)*bm*u_alpha,0.0,1.0),1.0));
})";

// Convert SSBO (fixed-point uint) → scanTex (RGBA16F)
static const char* copySSBOComp=R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(rgba16f,binding=0)writeonly uniform image2D u_img;
uniform ivec2 u_cs;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  int bi=(p.y*u_cs.x+p.x)*3;
  imageStore(u_img,p,vec4(vec3(float(d[bi])/65535.0,float(d[bi+1])/65535.0,float(d[bi+2])/65535.0),0));
})";

static GLuint cs(const char* s,GLenum t){
  GLuint sh=glCreateShader(t);glShaderSource(sh,1,&s,0);glCompileShader(sh);
  GLint ok=0;glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);if(!ok){char l[4096];GLsizei n=0;glGetShaderInfoLog(sh,sizeof(l),&n,l);fprintf(stderr,"[gl] Error:\n%s\n",l);}return sh;}
static GLuint lp(const char* l,GLuint v,GLuint f){
  GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
  if(!ok){char b[4096];GLsizei n=0;glGetProgramInfoLog(p,sizeof(b),&n,b);fprintf(stderr,"[gl] %s err:\n%s\n",l,b);}else fprintf(stderr,"[gl] %s OK\n",l);return p;}
static GLuint ct(GLenum fmt,int w,int h,GLenum f,GLenum t){
  GLuint x;glGenTextures(1,&x);glBindTexture(GL_TEXTURE_2D,x);glTexImage2D(GL_TEXTURE_2D,0,fmt,w,h,0,f,t,0);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);return x;}
static GLuint ctM(GLenum fmt,int w,int h,int lv){
  GLuint x;glGenTextures(1,&x);glBindTexture(GL_TEXTURE_2D,x);glTexStorage2D(GL_TEXTURE_2D,lv,fmt,w,h);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);return x;}

bool initRenderer(int w,int h){
  CW=w;CH=h;mipLevels=1;int d=std::max(w,h);while(d>1){d>>=1;mipLevels++;}if(mipLevels>2)mipLevels=2;
  if(!glfwInit())return false;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,4);glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
  glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);glfwWindowHint(GLFW_RESIZABLE,GLFW_TRUE);
  g_window=glfwCreateWindow(WinW,WinH,"v7",0,0);if(!g_window){glfwTerminate();return false;}
  glfwMakeContextCurrent(g_window);glfwSwapInterval(0);glewExperimental=GL_TRUE;glewInit();
  cvsColorTex=ct(GL_RGBA8,w,h,GL_RGBA,GL_UNSIGNED_BYTE);scanTex=ct(GL_RGBA16F,w,h,GL_RGBA,GL_HALF_FLOAT);
  renderTex=ct(GL_RGBA8,w,h,GL_RGBA,GL_UNSIGNED_BYTE);avgTex=ctM(GL_RGBA8,w,h,mipLevels);occTex=ctM(GL_R8,w,h,mipLevels);
  lvlTex=ct(GL_R8,w,h,GL_RED,GL_UNSIGNED_BYTE);
  for(int i=0;i<2;i++)accTex[i]=ct(GL_RGBA16F,w,h,GL_RGBA,GL_HALF_FLOAT);
  glGenBuffers(1,&scanSSBO);glBindBuffer(GL_SHADER_STORAGE_BUFFER,scanSSBO);
  glBufferData(GL_SHADER_STORAGE_BUFFER,(size_t)w*h*3*4,0,GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,scanSSBO);
  float verts[]={-1,-1,0,0,3,-1,2,0,-1,3,0,2};
  glGenVertexArrays(1,&fullVAO);glBindVertexArray(fullVAO);GLuint vbo;glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
  glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,(void*)0);
  glEnableVertexAttribArray(1);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,16,(void*)8);glBindVertexArray(0);
  auto lc=[&](const char* s,GLuint& p,const char* l){GLuint c=cs(s,GL_COMPUTE_SHADER);p=glCreateProgram();glAttachShader(p,c);glLinkProgram(p);
    GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char b[4096];GLsizei n=0;glGetProgramInfoLog(p,sizeof(b),&n,b);fprintf(stderr,"[gl] %s err:\n%s\n",l,b);}else fprintf(stderr,"[gl] %s OK\n",l);glDeleteShader(c);};
  lc(clearComp,clearProg,"clear");lc(scanComp,scanProg,"scan");lc(blendComp,blendProg,"blend");
  lc(mipAvComp,mipAvProg,"mipAv");lc(mipOccComp,mipOccProg,"mipOcc");lc(occInitComp,occInitProg,"occInit");lc(lvlComp,lvlProg,"lvl");
  lc(accComp,accProg,"acc");lc(ditherComp,ditherProg,"dither");lc(copySSBOComp,copySSBOProg,"copySSBO");
  // cache the uniform locations of the per-frame passes (was a string lookup several times a frame)
  uScan.cc=glGetUniformLocation(scanProg,"u_cc");   uScan.cs=glGetUniformLocation(scanProg,"u_cs");
  uScan.amb=glGetUniformLocation(scanProg,"u_ambient");uScan.ndirs=glGetUniformLocation(scanProg,"u_ndirs");
  uScan.cp=glGetUniformLocation(scanProg,"u_cp");   uScan.emit=glGetUniformLocation(scanProg,"u_emit");
  uCopy.cs=glGetUniformLocation(copySSBOProg,"u_cs");
  uBlend.cc=glGetUniformLocation(blendProg,"u_cc"); uBlend.scan=glGetUniformLocation(blendProg,"u_scan");
  uBlend.cs=glGetUniformLocation(blendProg,"u_cs"); uBlend.emit=glGetUniformLocation(blendProg,"u_emit");
  uDither.src=glGetUniformLocation(ditherProg,"u_src");uDither.res=glGetUniformLocation(ditherProg,"u_res");
  uDither.alpha=glGetUniformLocation(ditherProg,"u_alpha");uDither.frame=glGetUniformLocation(ditherProg,"u_frame");
  GLuint dssbo;glGenBuffers(1,&dssbo);dirSSBO=dssbo;
  glBindBuffer(GL_SHADER_STORAGE_BUFFER,dirSSBO);
  glBufferData(GL_SHADER_STORAGE_BUFFER,8,0,GL_DYNAMIC_DRAW);   // sized properly by ensureDirTable()
  {GLuint v=cs(vertSrc,GL_VERTEX_SHADER),f=cs(fullFrag,GL_FRAGMENT_SHADER);rawProg=lp("rawFrag",v,f);glDeleteShader(v);glDeleteShader(f);}
  {GLuint v=cs(vertSrc,GL_VERTEX_SHADER),f=cs(mipFrag,GL_FRAGMENT_SHADER);mipProg=lp("mipFrag",v,f);glDeleteShader(v);glDeleteShader(f);}
  {GLenum e=glGetError();if(e!=GL_NO_ERROR)fprintf(stderr,"[gl] initRenderer error 0x%04X\n",e);}
  return true;}

// Build/refresh the direction table for the current g_circleR and upload it once.
static void ensureDirTable(){
  if(g_dirsR==g_circleR&&!g_dirs.empty())return;
  g_dirs.clear();
  int R=g_circleR,m=1-R,x=0,y=R;
  while(x<y){
    int px[8]={x,-x,x,-x,y,-y,y,-y};
    int py[8]={y,y,-y,-y,x,x,-x,-x};
    for(int i=0;i<8;i++){g_dirs.push_back(px[i]);g_dirs.push_back(py[i]);}
    x++;if(m<0)m+=2*x+1;else{y--;m+=2*(x-y)+1;}
  }
  if((int)(g_dirs.size()/2)>65535)g_dirs.resize((size_t)65535*2);   // compute z-dimension limit
  g_dirsR=g_circleR;
  if(!dirSSBO){glGenBuffers(1,&dirSSBO);}
  glBindBuffer(GL_SHADER_STORAGE_BUFFER,dirSSBO);
  glBufferData(GL_SHADER_STORAGE_BUFFER,g_dirs.size()*sizeof(GLint),g_dirs.data(),GL_STATIC_DRAW);
}

void uploadCanvasTexture(const Pixel* p,int W,int H){
  if(!g_texDirty)return;
  int x0=g_texX0,y0=g_texY0,x1=g_texX1,y1=g_texY1;
  if(g_texAll){x0=0;y0=0;x1=W-1;y1=H-1;g_texAll=false;}
  else if(x1<x0||y1<y0){g_texDirty=false;return;}          // nothing changed this frame
  if(x0<0)x0=0;if(y0<0)y0=0;if(x1>W-1)x1=W-1;if(y1>H-1)y1=H-1;
  int bw=x1-x0+1,bh=y1-y0+1;if(bw<=0||bh<=0){g_texDirty=false;return;}
  static std::vector<unsigned char> b;b.resize((size_t)bw*bh*4);
  for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
    const Pixel& q=p[(size_t)y*W+x];int o=(((y-y0)*bw)+(x-x0))*4;
    b[o+0]=(unsigned char)(std::max(0.f,std::min(q.r,1.f))*255);
    b[o+1]=(unsigned char)(std::max(0.f,std::min(q.g,1.f))*255);
    b[o+2]=(unsigned char)(std::max(0.f,std::min(q.b,1.f))*255);
    b[o+3]=(unsigned char)(std::max(0.f,std::min(q.a,1.f))*255);}
  glBindTexture(GL_TEXTURE_2D,cvsColorTex);glTexSubImage2D(GL_TEXTURE_2D,0,x0,y0,bw,bh,GL_RGBA,GL_UNSIGNED_BYTE,b.data());g_texDirty=false;}

void renderScanline(int W,int H){
  static int frameIdx=0,accPing=0;
  ensureDirTable();
  const int nd=(int)(g_dirs.size()/2);
  // 1. Clear the accumulation buffer on the GPU (no CPU->GPU upload per frame)
  size_t sz=(size_t)W*H*3*sizeof(GLuint);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER,scanSSBO);
  glClearBufferSubData(GL_SHADER_STORAGE_BUFFER,GL_R32UI,0,(GLsizeiptr)sz,GL_RED_INTEGER,GL_UNSIGNED_INT,0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,scanSSBO);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,dirSSBO);
  // 2. ONE dispatch for every direction: the index arrives as gl_GlobalInvocationID.z
  //    (was one glDispatchCompute per direction, i.e. 728 tiny launches per frame).
  glUseProgram(scanProg);
  glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,cvsColorTex);glUniform1i(uScan.cc,0);
  float ambient[3]={g_wallClr[0],g_wallClr[1],g_wallClr[2]};
  glUniform2i(uScan.cs,W,H);glUniform3fv(uScan.amb,1,ambient);
  glUniform1i(uScan.ndirs,nd);glUniform1f(uScan.cp,1.f);glUniform1f(uScan.emit,g_emit);
  {int maxrc=2*std::max(W,H)+1;glDispatchCompute((maxrc+255)/256,1,(GLuint)nd);}
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  // 4. Convert SSBO → scanTex (RGBA16F)
  glUseProgram(copySSBOProg);
  glUniform2i(uCopy.cs,W,H);
  glBindImageTexture(0,scanTex,0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA16F);
  glDispatchCompute((W+7)/8,(H+7)/8,1);glMemoryBarrier(GL_ALL_BARRIER_BITS); // one barrier at end for next passes
  // 4. Blend
  glUseProgram(blendProg);
  glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,cvsColorTex);glUniform1i(uBlend.cc,0);
  glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,scanTex);glUniform1i(uBlend.scan,1);
  glUniform2i(uBlend.cs,W,H);glUniform1f(uBlend.emit,g_emit);
  glBindImageTexture(0,renderTex,0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA8);
  glDispatchCompute((W+7)/8,(H+7)/8,1);glMemoryBarrier(GL_ALL_BARRIER_BITS);
  // 5a. Temporal accumulation
  if(g_useAcc){
    int accPong=1-accPing;
    if(frameIdx==0)glCopyImageSubData(renderTex,GL_TEXTURE_2D,0,0,0,0,accTex[0],GL_TEXTURE_2D,0,0,0,0,W,H,1);
    glUseProgram(accProg);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,renderTex);
    glUniform1i(glGetUniformLocation(accProg,"u_new"),0);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,accTex[accPing]);
    glUniform1i(glGetUniformLocation(accProg,"u_old"),1);
    glUniform1f(glGetUniformLocation(accProg,"u_a"),0.25f);
    glBindImageTexture(0,accTex[accPong],0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA16F);
    glDispatchCompute((W+7)/8,(H+7)/8,1);glMemoryBarrier(GL_ALL_BARRIER_BITS);
    accPing=accPong;frameIdx++;
  }
  // 5b. Dither: read from acc (if avail) or renderTex, write to renderTex.
  //     With no accumulation source and noise off this pass is renderTex -> renderTex, i.e. a no-op.
  if(g_useAcc||g_noise>0.001f){
    GLuint src=g_useAcc?accTex[accPing]:renderTex;
    glUseProgram(ditherProg);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,src);
    glUniform1i(uDither.src,0);
    glUniform2i(uDither.res,W,H);
    glUniform1f(uDither.alpha,g_noise>0.001f?0.8f:0.0f);
    glUniform1i(uDither.frame,frameIdx);
    glBindImageTexture(0,renderTex,0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA8);
    glDispatchCompute((W+7)/8,(H+7)/8,1);glMemoryBarrier(GL_ALL_BARRIER_BITS);
  }
  // 5c. Mip chain + level (only when mip display enabled)
  if(g_useMip){
  glUseProgram(occInitProg);
  glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,cvsColorTex);glUniform1i(glGetUniformLocation(occInitProg,"u_scene"),0);
  glUniform2i(glGetUniformLocation(occInitProg,"u_cs"),W,H);
  glBindImageTexture(0,occTex,0,GL_FALSE,0,GL_WRITE_ONLY,GL_R8);
  glDispatchCompute((W+7)/8,(H+7)/8,1);glMemoryBarrier(GL_ALL_BARRIER_BITS);
  glCopyImageSubData(renderTex,GL_TEXTURE_2D,0,0,0,0,avgTex,GL_TEXTURE_2D,0,0,0,0,W,H,1);
  int mw=W,mh=H,srcTex=renderTex;
  for(int l=1;l<mipLevels;l++){
    int smw=std::max(1,mw),smh=std::max(1,mh);mw=std::max(1,mw/2);mh=std::max(1,mh/2);
    glUseProgram(mipAvProg);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,srcTex);
    glUniform1i(glGetUniformLocation(mipAvProg,"u_src"),0);
    glUniform2i(glGetUniformLocation(mipAvProg,"u_sz"),smw,smh);glUniform1i(glGetUniformLocation(mipAvProg,"u_lod"),l-1);
    glBindImageTexture(0,avgTex,l,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA8);
    glDispatchCompute((mw+7)/8,(mh+7)/8,1);glMemoryBarrier(GL_ALL_BARRIER_BITS);
    srcTex=avgTex;
  }
  mw=W;mh=H;glUseProgram(mipOccProg);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,occTex);
  glUniform1i(glGetUniformLocation(mipOccProg,"u_src"),0);
  for(int l=1;l<mipLevels;l++){
    int smw=std::max(1,mw),smh=std::max(1,mh);mw=std::max(1,mw/2);mh=std::max(1,mh/2);
    glUniform2i(glGetUniformLocation(mipOccProg,"u_sz"),smw,smh);glUniform1i(glGetUniformLocation(mipOccProg,"u_lod"),l-1);
    glBindImageTexture(0,occTex,l,GL_FALSE,0,GL_WRITE_ONLY,GL_R8);
    glDispatchCompute((mw+7)/8,(mh+7)/8,1);glMemoryBarrier(GL_ALL_BARRIER_BITS);
  }
  glUseProgram(lvlProg);
  glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,occTex);glUniform1i(glGetUniformLocation(lvlProg,"u_occ"),0);
  glUniform2i(glGetUniformLocation(lvlProg,"u_res"),W,H);glUniform1i(glGetUniformLocation(lvlProg,"u_mips"),mipLevels);
  glBindImageTexture(0,lvlTex,0,GL_FALSE,0,GL_WRITE_ONLY,GL_R8);
  glDispatchCompute((W+7)/8,(H+7)/8,1);glMemoryBarrier(GL_ALL_BARRIER_BITS);
  }
  {static GLenum lastErr=GL_NO_ERROR;GLenum e=glGetError();
   if(e!=GL_NO_ERROR&&e!=lastErr)fprintf(stderr,"[gl] renderScanline error 0x%04X\n",e);lastErr=e;}
}

// Fit mode: CROP (cover). The canvas is scaled so that it fills the whole window and
// the overflowing axis is cropped, instead of letterboxing it. Both display() and
// scr2mem() go through this one helper, so what you see and where you draw stay in sync
// for any window shape.
//   du,dv = extents (in canvas UV) of the visible region at the current camera zoom.
//   g_cam.sk keeps its original meaning: the visible fraction, sk == 1 => cover fit,
//   LARGER sk => zoomed OUT (so Ctrl+Up = zoom in, as documented).
// At sk == 1 the axis with the larger window/canvas ratio shows exactly the whole
// canvas and the other one shows less. Pixels stay square for every window aspect and
// every zoom, because du/dv derive from the same pixel ratio (pixels per cell = M/sk
// on both axes).
static void viewScaleUV(int fbW,int fbH,float& du,float& dv){
  float a=(CW>0)?(float)fbW/(float)CW:1.0f;
  float b=(CH>0)?(float)fbH/(float)CH:1.0f;
  float m=(a>b)?a:b;
  if(!(m>0.0f)){du=dv=1.0f;return;}
  float z=(float)g_cam.sk;if(!(z>0.0f))z=1.0f;
  du=(a/m)*z;dv=(b/m)*z;
}

void display(int,int){
  int fbW,fbH;glfwGetFramebufferSize(g_window,&fbW,&fbH);
  if(fbW<=0||fbH<=0)return;
  glViewport(0,0,fbW,fbH);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  float du,dv;viewScaleUV(fbW,fbH,du,dv);
  if(g_useMip){
    glUseProgram(mipProg);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,avgTex);glUniform1i(glGetUniformLocation(mipProg,"u_avg"),0);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,lvlTex);glUniform1i(glGetUniformLocation(mipProg,"u_lvl"),1);
    glUniform2f(glGetUniformLocation(mipProg,"u_cp"),float(g_cam.x),float(g_cam.y));
    glUniform2f(glGetUniformLocation(mipProg,"u_uv"),du,dv);
    glUniform2f(glGetUniformLocation(mipProg,"u_cv"),float(CW),float(CH));
  }else{
    glUseProgram(rawProg);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,renderTex);glUniform1i(glGetUniformLocation(rawProg,"u_t"),0);
    glUniform2f(glGetUniformLocation(rawProg,"u_cp"),float(g_cam.x),float(g_cam.y));
    glUniform2f(glGetUniformLocation(rawProg,"u_uv"),du,dv);
  }
  glBindVertexArray(fullVAO);glDrawArrays(GL_TRIANGLES,0,3);glfwSwapBuffers(g_window);}

void scr2mem(double sx,double sy,int& mx,int& my,int W,int H){
  // Cursor positions and glfwGetWindowSize are both in screen coordinates, while the
  // viewport uses framebuffer pixels; normalising by the window size cancels the DPI
  // scale between the two (the old code mixed them, which broke drawing at 125/150%).
  int winW,winH;glfwGetWindowSize(g_window,&winW,&winH);
  int fbW,fbH;glfwGetFramebufferSize(g_window,&fbW,&fbH);
  if(winW<=0||winH<=0){mx=0;my=0;return;}
  if(fbW<=0||fbH<=0){fbW=winW;fbH=winH;}
  double u=sx/(double)winW,v=1.0-sy/(double)winH;
  float du,dv;viewScaleUV(fbW,fbH,du,dv);
  double abx=(double)du*(u-0.5)+g_cam.x;double aby=(double)dv*(v-0.5)+g_cam.y;
  mx=(int)(abx*W);my=(int)(aby*H);mx=std::max(0,std::min(mx,W-1));my=std::max(0,std::min(my,H-1));}
void screenshot(){static int c=0;c++;std::string fn=std::to_string(c)+"_screenshot.tga";
  int fbW,fbH;glfwGetFramebufferSize(g_window,&fbW,&fbH);if(fbW<=0||fbH<=0)return;
  std::vector<unsigned char> buf((size_t)fbW*fbH*3);
  glPixelStorei(GL_PACK_ALIGNMENT,1);
  glReadPixels(0,0,fbW,fbH,GL_BGR,GL_UNSIGNED_BYTE,buf.data());
  FILE* f=fopen(fn.c_str(),"wb");if(f){unsigned char hdr[18]={0};hdr[2]=2;hdr[12]=fbW&255;hdr[13]=fbW>>8;hdr[14]=fbH&255;hdr[15]=fbH>>8;hdr[16]=24;hdr[17]=0x20;
  fwrite(hdr,1,18,f);fwrite(buf.data(),1,buf.size(),f);fclose(f);}}
