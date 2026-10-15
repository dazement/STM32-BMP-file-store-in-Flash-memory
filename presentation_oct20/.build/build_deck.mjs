import fs from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { FileBlob, Presentation, PresentationFile } from '@oai/artifact-tool';

const W='/Users/minho/gradproj/bmp_gray_practice/presentation_oct20';
const SKILL='/Users/minho/.codex/plugins/cache/openai-primary-runtime/presentations/26.1011.11113/skills/presentations';
const PY='/Users/minho/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3';
const FONT='Apple SD Gothic Neo', INK='#14304B', BLUE='#166B85', MUTED='#52677A';
const pres=Presentation.create({slideSize:{width:1280,height:720}});
const sources={
  st:'https://www.st.com/resource/en/datasheet/stm32f746ng.pdf',
  board:'https://www.st.com/resource/en/user_manual/um1907-discovery-kit-for-stm32f7-series-with-stm32f746ng-mcu-stmicroelectronics.pdf',
  winbond:'https://media.digikey.com/pdf/Data%20Sheets/Winbond%20PDFs/W25Q32JV_RevI_5-4-21.pdf',
  code:'/Users/minho/gradproj/bmp_gray_practice/Core/Src/',
  evidence:W+'/output/evidence_existing.json',
  chip:'/Users/minho/gradproj/f746_bmp_gray/docs/hardware_setup.md',
  csv:'/Users/minho/gradproj/bmp_gray_practice/pc/results/timings.csv'
};
function text(s,value,x,y,w,h,size=27,color=INK,bold=false){
  const sh=s.shapes.add({geometry:'textbox',position:{left:x,top:y,width:w,height:h},fill:'none',line:{fill:'none',width:0}});
  sh.text=value;
  sh.text.style={typeface:FONT,fontSize:size,color,bold,autoFit:'none',wrap:'square',insets:0,verticalAlignment:'top'};
  return sh;
}
function slide(title,notes){
  const s=pres.slides.add(); s.background.fill='#FFFFFF';
  text(s,title,72,50,1136,66,44,INK,true);
  text(s,String(pres.slides.items.length).padStart(2,'0'),1175,675,40,26,18,MUTED);
  s.speakerNotes.text=notes;
  return s;
}
function table(s,values,{x=72,y=174,w=1136,h=390,widths,size=25}={}){
  const t=s.tables.add({rows:values.length,columns:values[0].length,left:x,top:y,width:w,height:h,values,columnWidths:widths});
  t.borders.assign({style:'solid',fill:'#D6E0E8',width:1});
  for(let r=0;r<values.length;r++){
    t.rows[r].height=h/values.length;
    for(let c=0;c<values[0].length;c++){
      const cell=t.getCell(r,c);
      cell.fill=r===0?INK:(r%2===0?'#F1F6F9':'#FFFFFF');
      cell.text.style={typeface:FONT,fontSize:size,color:r===0?'#FFFFFF':INK,bold:r===0,verticalAlignment:'middle',alignment:'left',insets:12};
    }
  }
  return t;
}
async function image(s,name,x,y,w,h,fit='contain'){
  const data=await fs.readFile(W+'/.build/assets/'+name+'.png');
  s.images.add({blob:data.buffer.slice(data.byteOffset,data.byteOffset+data.byteLength),contentType:'image/png',alt:name,position:{left:x,top:y,width:w,height:h},fit});
}

// 1. Minimal cover. The color strip is the real test image used as a background.
{
const s=pres.slides.add();s.background.fill='#FFFFFF';
await image(s,'original',840,0,440,720,'cover');
text(s,'UART 및 SPI Flash\n기본 기능 구현',72,170,740,180,60,INK,true);
text(s,'STM32F746G-DISCO',76,380,650,50,31,BLUE);
text(s,'2차 발표\n2026년 10월 20일',76,520,620,84,28,MUTED);
s.speakerNotes.text='이번 발표에서는 PC에서 보낸 대용량 BMP를 SPI Flash에 저장하고 PC에 원본 그대로 복원하는 기능을 설명합니다. 표지 배경은 실제 테스트 입력 BMP입니다.\n출처: '+sources.evidence;
}
// 2. Scope
{
const s=slide('이번 Demo의 확인 항목','파일 헤더와 픽셀, 행 패딩까지 전체 파일을 저장하고 복원합니다. 화면 확인과 데이터 무결성 확인을 함께 수행합니다. 회색조 출력은 추가 기능입니다.\n출처: 사용자 제공 2차 발표 요구사항, '+sources.evidence);
text(s,'3 MiB 이상의 BMP 전체를 Flash에 저장하고 원본으로 복원',72,120,1136,42,28,BLUE);
table(s,[['발표 항목','확인 방법'],['PC와 STM32 UART 통신','8N1 바이너리 전송, DMA 수신 및 송신'],['STM32와 SPI Flash 통신','JEDEC ID와 상태 레지스터 확인'],['Flash Read / Write','블록 삭제, 페이지 쓰기, 주소 기반 재읽기'],['3 MB 이상 전송 및 저장','3,145,782바이트 BMP 전체 저장'],['저장 데이터 복원','Flash에서 읽어 PC에 BMP 파일로 저장'],['무결성','CRC32, SHA-256, 전체 바이트 비교']],{h:420,widths:[370,766]});
}
// 3. Hardware and requirement discrepancy
{
const s=slide('실제 하드웨어와 과제 조건','기존 실물 확인 기록은 W25Q128, EF4018입니다. W25Q32 실물을 시험한 것으로 설명하지 않습니다. 과제에서 W25Q32를 반드시 요구하면 외부 칩으로 실물 시험이 필요하거나 담당자의 대체 허용이 필요합니다.\n출처: '+sources.chip+'\n'+sources.board+'\n'+sources.winbond);
table(s,[['항목','현재 확인 기록','과제 조건'],['MCU / 보드','STM32F746G-DISCO','STM32 MCU'],['Flash 모델','W25Q128','W25Q32'],['물리 용량','16 MiB','4 MiB'],['JEDEC ID','EF 40 18','EF 40 16'],['이미지 영역','첫 4 MiB 안으로 제한','3 MB 이상 전체 저장']],{h:372,widths:[250,443,443]});
text(s,'현재 칩 대체 허용 여부를 확인해야 합니다',72,588,1136,48,31,'#A64F16',true);
}
// 4. UART
{
const s=slide('UART 8N1과 Double Buffer','UART 데이터 단위 8비트와 BMP 픽셀 형식 8비트를 구분합니다. 원본은 24비트 BMP이며, 파일의 바이트를 8N1로 전송합니다. DMA M0/M1은 각 4096바이트입니다. 부분 수신도 NDTR로 처리하고 완료 콜백으로 은행 전환을 추적합니다. TX는 DMA 이중 버퍼입니다.\n출처: '+sources.code+'uart_dma_rx.c, uart_dma_tx.c, uart_flash_test.c\n'+sources.board);
table(s,[['항목','구현'],['PC 연결','ST-LINK 가상 COM, USART1 PA9 / PB7'],['UART 프레임','데이터 8비트, 패리티 없음, 정지 1비트'],['확인한 비교 실험 속도','921600 baud'],['수신 버퍼','4 KiB 두 개, 하드웨어 Double Buffer Mode'],['송신 버퍼','4 KiB 두 개, UART TX DMA'],['흐름 제어','4 KiB 청크 CRC, ACK, 최대 두 청크 전송']],{h:420,widths:[380,756],size:24});
text(s,'Flash 기록 중에도 다음 청크를 DMA로 수신',72,624,1136,40,28,BLUE,true);
}
// 5. SPI operations
{
const s=slide('SPI Flash Read / Write','QUADSPI는 사용한 MCU 주변장치입니다. 현재 명령, 주소, 데이터는 각각 한 선으로 전송하는 일반 SPI 1-1-1 프로토콜입니다. HCLK 216 MHz, prescaler 7이므로 SCK 27 MHz입니다. 매 쓰기 및 삭제 전에 WREN, BUSY 해제를 확인합니다.\n출처: '+sources.code+'flash_test.c\n'+sources.winbond);
text(s,'QUADSPI 컨트롤러, SPI 1-1-1, Mode 0, 27 MHz',72,120,1136,40,28,BLUE);
table(s,[['동작','명령','역할'],['칩 식별','9Fh','JEDEC ID 읽기'],['쓰기 허용','06h','Write Enable'],['상태 확인','05h','BUSY와 WEL 비트 확인'],['블록 삭제','D8h','64 KiB 단위 삭제'],['페이지 쓰기','02h','최대 256바이트, 페이지 경계 유지'],['읽기','03h','24비트 주소로 Flash 데이터 읽기']],{h:420,widths:[280,160,696],size:24});
}
// 6. Capacity and layout
{
const s=slide('대용량 이미지 저장 구조','3 MiB는 3,145,728바이트입니다. 입력 파일 3,145,782바이트는 3 MB와 3 MiB를 모두 넘습니다. 헤더부터 마지막 바이트까지 저장합니다. 삭제 범위를 64 KiB 경계로 올리므로 49개 블록, 쓰기는 총 12,289개 페이지에 해당합니다. 삭제 전에 전체 대상 영역을 PC에 백업합니다. 전체 이미지를 RAM에 보관하지 않습니다.\n출처: '+sources.evidence+'\n'+sources.code+'uart_flash_test.c\n'+sources.st);
table(s,[['항목','값'],['입력 BMP','1024 × 1024, 24비트 True Color'],['전체 파일 크기','3,145,782 B'],['이미지 주소 영역','0x000000 ~ 0x3EFFFF'],['실제 삭제 범위','3,211,264 B, 64 KiB 블록 49개'],['페이지 쓰기','256 B 단위, 총 12,289회'],['RAM 처리','작은 청크 버퍼 재사용, MCU SRAM 320 KiB']],{h:420,widths:[360,776],size:24});
text(s,'삭제할 범위는 저장 전에 PC에 백업',72,624,1136,40,28,BLUE,true);
}
// 7. Real image evidence
{
const s=slide('원본과 Flash 재읽기 파일이 일치','이 슬라이드의 두 이미지는 실제 원본과 기존 보드 재읽기 파일입니다. 파일 전체를 비교해 일치하고 CRC32와 SHA-256도 동일합니다. SHA-256: 0bfd1da377908544c1b1d2c0e10f8a3b53434448786bcac53d12b27c50f1dd8c. 현재 자료 작성 때 파일을 다시 검증했습니다. 최신 펌웨어 Demo는 별도로 실행하여 새 보고서를 남깁니다.\n출처: '+sources.evidence);
text(s,'PC 원본',183,126,300,36,29,INK,true);
text(s,'Flash 재읽기 파일',761,126,350,36,29,INK,true);
await image(s,'original',160,174,330,330);
await image(s,'restored',780,174,330,330);
table(s,[['확인 항목','원본','복원'],['파일 크기','3,145,782 B','3,145,782 B'],['CRC32','978AB7D9','978AB7D9']],{y:524,h:132,widths:[300,418,418],size:24});
text(s,'전체 바이트 일치, SHA-256 일치',72,668,1040,38,25,BLUE,true);
}
// 8. Live demo choreography
{
const s=slide('현장 Demo 실행 순서','프로젝트 폴더에서 python3 pc/demo_roundtrip.py를 실행합니다. 실제 칩 ID, 크기, CRC32, SHA-256, 전체 바이트 비교를 JSON으로 저장합니다. MCU를 RESET한 뒤 --read-only로 실행하면 PC 원본을 재전송하지 않고 Flash에서만 읽습니다. Serial Monitor를 종료해야 포트 점유 오류를 피합니다.\n출처: '+sources.code.replace('/Core/Src/','/pc/')+'demo_roundtrip.py');
const items=[['1','원본 BMP를 열어 크기와 이미지를 확인'],['2','Serial Monitor 종료, 새 펌웨어 실행'],['3','Demo 프로그램 실행, 백업과 저장 진행률 표시'],['4','PASS, CRC32, SHA-256 확인'],['5','복원 BMP를 열어 원본과 비교']];
items.forEach((item,i)=>{text(s,item[0],78,164+i*72,48,48,34,BLUE,true);text(s,item[1],150,166+i*72,1050,50,29,INK);});
text(s,'python3 pc/demo_roundtrip.py',150,560,1010,52,30,INK,true);
text(s,'리셋 후 읽기 전용 확인: 같은 명령에 --read-only 추가',150,618,1010,40,25,MUTED);
}
// 9. Evidence and outstanding chip condition
{
const s=slide('확인된 결과와 남은 조건','PASS는 기존 실물 출력 파일과 구현 근거에 대한 상태입니다. 오늘 수정한 TX DMA 및 8비트 고정 펌웨어는 빌드와 프로그램 검사를 통과했고, 현장 전 최신 펌웨어로 다시 실물 Demo를 실행해야 합니다. W25Q32 실물 요구는 현재 W25Q128 시험과 구분합니다.\n출처: '+sources.evidence+'\n'+sources.chip);
table(s,[['확인 항목','근거 / 상태'],['3 MiB 이상 원본 복원','3,145,782 B, 기존 결과 파일 일치'],['데이터 무결성','CRC32, SHA-256, 전체 바이트 비교 일치'],['UART Double Buffer','수신 전환 기록, 이전 실험 버퍼 초과 0회'],['저장 유지 증명','RESET 후 읽기 전용 Demo 준비'],['실제 W25Q32 사용','현재 W25Q128, 대체 조건 확인 필요']],{h:390,widths:[360,776],size:24});
text(s,'발표 직전 새 Demo 보고서로 결과를 갱신',72,608,1136,44,30,BLUE,true);
}
// 10. Extra implemented grayscale feature; historical single-run evidence.
{
const s=slide('추가 기능: 8비트 Gray 출력','필수 Demo는 원본 그대로 복원입니다. 별도 기능인 회색조 출력은 현재 8비트로 고정했습니다. 비교 데이터는 이전 버전에서 같은 입력과 921600 baud로 각 1회 측정한 기록입니다. 입력 전송 시작부터 Gray 마지막 바이트 수신까지이며 백업 준비 44.18초는 별도입니다. 총시간 감소율 (95.600607-72.772918)/95.600607 = 23.878%입니다. 새 버전 반복 측정 결과로 일반화하지 않습니다.\n출처: '+sources.csv+'\n'+sources.evidence);
await image(s,'gray8',72,176,350,350);
text(s,'23.9% 시간 감소',470,152,720,64,44,BLUE,true);
table(s,[['이전 비교 기록','24비트 Gray','8비트 Gray'],['출력 크기','3,145,782 B','1,049,654 B'],['전체 시간','95.60 s','72.77 s']],{x:470,y:262,w:738,h:216,widths:[245,246,247],size:24});
text(s,'현재 운용 출력은 8비트 Gray로 고정',470,516,738,48,27,INK,true);
text(s,'각 1회 측정, 921600 baud, 백업 준비 제외',72,618,1136,42,25,MUTED);
}

await fs.mkdir(W+'/.build/previews',{recursive:true});
const candidate=W+'/.build/candidate_v2.pptx';
await (await PresentationFile.exportPptx(pres)).save(candidate);
const {finalizePresentation}=await import(pathToFileURL(path.join(SKILL,'container_tools/artifact_tool_utils.mjs')).href);
const final=W+'/output/2차발표_UART_SPIFlash_10월20일.pptx';
const result=await finalizePresentation({workspaceDir:W,candidatePath:candidate,finalPath:final,
  pythonExecutable:PY,
  integrityValidatorPath:path.join(SKILL,'container_tools/inspect_presentation_package_integrity.py'),
  layoutValidatorPath:path.join(SKILL,'container_tools/inspect_presentation_layout_geometry.py'),
  layoutArgs:['--expected-slide-size-emu','12192000,6858000','--validate-bullet-geometry','--validate-heading-fit',
    ...[2,3,4,5,6,7,9,10].flatMap(n=>['--require-native-table-slide',String(n)])],
  explicitTotalSlideCount:10,requiredNativeTableOwnerSlides:[2,3,4,5,6,7,9,10],
  fontPolicy:{basis:'design',families:[FONT]},verifyArtifactToolImport:true,
  receiptPath:W+'/.build/validation.json'});
console.log('Final:',final,JSON.stringify(result).slice(0,1500));
const rendered=await PresentationFile.importPptx(await FileBlob.load(final));
for(let i=0;i<rendered.slides.items.length;i++){
  const png=await rendered.export({slide:rendered.slides.items[i],format:'png',scale:1});
  await fs.writeFile(W+'/.build/previews/slide-'+String(i+1).padStart(2,'0')+'.png',new Uint8Array(await png.arrayBuffer()));
}
