const fs=require('fs');const path=require('path');const os=require('os');
const root=path.resolve(process.argv[2]||'outputs/conflux-brand-pack');
const fontwork=fs.mkdtempSync(path.join(os.tmpdir(),'conflux-fonts-'));
const xml=s=>s.replaceAll('&','&amp;').replaceAll('<','&lt;');
fs.writeFileSync(path.join(fontwork,'fonts.conf'),`<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd"><fontconfig><dir>${xml(path.join(root,'assets/fonts'))}</dir><cachedir>${xml(fontwork)}</cachedir></fontconfig>`);
process.env.FONTCONFIG_FILE=path.join(fontwork,'fonts.conf');
let sharp;try{sharp=require('sharp')}catch{sharp=require('/home/kyau/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp')}
const jobs=[];
function png(src,dest,width,height){jobs.push(async()=>{fs.mkdirSync(path.dirname(dest),{recursive:true});let s=sharp(src,{density:300}).resize(width,height,{fit:'contain',background:'#00000000'});await s.png().toFile(dest);});}
for(const name of fs.readdirSync(path.join(root,'assets/logos'))){if(!name.endsWith('.svg'))continue;const width=name.includes('symbolic')?96:name.includes('symbol')?1024:2048;png(path.join(root,'assets/logos',name),path.join(root,'assets/logos',name.replace('.svg','.png')),width);}
for(const theme of ['dark','light']){
 const source=path.join(root,`assets/app-icons/conflux-app-${theme}.svg`);
 for(const size of [16,22,24,32,48,64,128,256,512,1024])png(size<32?path.join(root,`assets/app-icons/conflux-app-${theme}-small.svg`):source,path.join(root,`assets/app-icons/conflux-app-${theme}-${size}.png`),size,size);
 png(source,path.join(root,`assets/social/conflux-avatar-${theme}.png`),1024,1024);
 for(const type of ['conflux-open-graph','social-banner']){
  const w=type==='conflux-open-graph'?1200:1500,h=type==='conflux-open-graph'?630:500;
  png(path.join(root,`assets/social/${type}-${theme}.svg`),path.join(root,`assets/social/${type}-${theme}.png`),w,h);
 }
 for(const n of fs.readdirSync(path.join(root,'assets/ui-icons'))){if(!n.endsWith('.svg')||n==='sprite.svg')continue;jobs.push(async()=>{const p=path.join(root,`assets/ui-icons/png-${theme}`,n.replace('.svg','.png'));fs.mkdirSync(path.dirname(p),{recursive:true});const s=fs.readFileSync(path.join(root,'assets/ui-icons',n),'utf8').replaceAll('currentColor',theme==='dark'?'#F5F4F0':'#24252B');await sharp(Buffer.from(s)).resize(48,48).png().toFile(p);});}
}
for(const size of [16,32,48])png(path.join(root,`assets/app-icons/conflux-app-dark${size===16?'-small':''}.svg`),path.join(root,`assets/favicons/favicon-${size}.png`),size,size);
png(path.join(root,'assets/app-icons/conflux-app-dark.svg'),path.join(root,'assets/favicons/apple-touch-icon.png'),180,180);
for(const size of [192,512])png(path.join(root,'assets/app-icons/conflux-app-dark.svg'),path.join(root,`assets/favicons/android-chrome-${size}x${size}.png`),size,size);
png(path.join(root,'assets/favicons/maskable-dark.svg'),path.join(root,'assets/favicons/maskable-512x512.png'),512,512);
(async()=>{for(let i=0;i<jobs.length;i+=6)await Promise.all(jobs.slice(i,i+6).map(f=>f()));
 const linux=path.join(root,'integration/linux/hicolor');
 for(const size of [16,22,24,32,48,64,128,256,512,1024]){const dest=path.join(linux,`${size}x${size}/apps`);fs.mkdirSync(dest,{recursive:true});fs.copyFileSync(path.join(root,`assets/app-icons/conflux-app-dark-${size}.png`),path.join(dest,'conflux.png'));}
 for(const [theme,src] of [['scalable','assets/app-icons/conflux-app-dark.svg'],['symbolic','assets/logos/conflux-symbolic.svg']]){const dest=path.join(linux,`${theme}/apps`);fs.mkdirSync(dest,{recursive:true});fs.copyFileSync(path.join(root,src),path.join(dest,theme==='symbolic'?'conflux-symbolic.svg':'conflux.svg'));}
 const bitmaps=[16,32,48].map(size=>fs.readFileSync(path.join(root,`assets/favicons/favicon-${size}.png`)));
 const head=Buffer.alloc(6+16*bitmaps.length);head.writeUInt16LE(1,2);head.writeUInt16LE(bitmaps.length,4);let offset=head.length;
 bitmaps.forEach((buf,i)=>{const at=6+i*16;head[at]=[16,32,48][i];head[at+1]=head[at];head.writeUInt16LE(1,at+4);head.writeUInt16LE(32,at+6);head.writeUInt32LE(buf.length,at+8);head.writeUInt32LE(offset,at+12);offset+=buf.length});
 fs.writeFileSync(path.join(root,'assets/favicons/favicon.ico'),Buffer.concat([head,...bitmaps]));
 if(path.resolve(__filename)!==path.join(root,'source/rasterize.cjs'))fs.copyFileSync(__filename,path.join(root,'source/rasterize.cjs'));
 fs.rmSync(fontwork,{recursive:true,force:true});
 console.log(`Exported ${jobs.length} PNGs plus Linux hicolor assets.`);
})().catch(e=>{console.error(e);process.exit(1)});
