"""Render schematic UI references, NOT device screenshots. Run on macOS with Pillow."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).parent
FONT = '/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc'
font = ImageFont.truetype(FONT, 16)
small = ImageFont.truetype(FONT, 12)
screens = [
 ('home','通常',['メニュー','話しかける','設定']),
 ('menu','メニュー',['前へ','決定','次へ']),
 ('settings','設定の項目選択',['前の項目','決定','次の項目']),
 ('adjust','LEDの調整',['－','戻る','＋']),
 ('news','ニュース読み上げ',['文字表示','停止','—']),
 ('busy','音声入力・処理中',['—','処理中','—']),
]
for name,title,labels in screens:
    im=Image.new('RGB',(320,270),'#18222d'); d=ImageDraw.Draw(im)
    d.text((6,3),'v9.34',font=small,fill='white')
    d.text((6,35),'顔写真表示エリア（省略）',font=font,fill='#97a6b5')
    if name=='menu':
        d.rounded_rectangle((6,72,181,193),radius=9,fill='black',outline='white')
        d.polygon([(13,61),(20,73),(41,73)],fill='white')
        for i,label in enumerate(['一般ニュース','AI関連ニュース','芸能ニュース','経済ニュース','その他','設定']):
            d.text((26,76+i*19),label,font=font,fill='white')
        d.polygon([(13,83),(13,91),(20,87)],fill='white')
    if name in ('settings','adjust'):
        d.rounded_rectangle((10,52,309,187),radius=8,fill='black',outline='white')
        d.text((22,58),'設定',font=font,fill='white')
        for y,label,level in [(84,'音量',2),(124,'LED',4)]:
            d.text((22,y),label,font=font,fill='white')
            for i in range(6):
                d.rectangle((72+i*16,y+2,82+i*16,y+12),fill='white' if i<level else '#404040')
        d.rectangle((16,155,303,191),fill='black')
        d.text((22,160),'LED' if name=='adjust' else '音量',font=font,fill='white')
        d.text((160,160),'調整中' if name=='adjust' else '選択中',font=font,fill='white')
    if name=='news':
        d.ellipse((130,119,310,161),fill='white')
        d.text((162,128),'ニュースです',font=font,fill='black')
        d.rounded_rectangle((0,164,319,207),radius=6,fill='black')
        d.text((8,166),'経済ニュース（サンプル）',font=font,fill='white')
        d.text((8,184),'記事の字幕がここに表示されます',font=font,fill='white')
    if name=='busy':
        d.text((25,105),'音声入力・処理中',font=font,fill='white')
    d.rectangle((0,208,319,239),fill='black')
    d.line((0,208,319,208),fill='#808080')
    for i,label in enumerate(labels):
        if i: d.line((i*107,212,i*107,237),fill='#606060')
        d.text((i*107+7,214),label,font=font,fill='#606060' if name=='busy' or label=='—' else 'white')
    d.rectangle((0,240,319,269),fill='#e8edf2')
    d.text((8,246),'画面イメージ・実機キャプチャではありません',font=small,fill='#253242')
    im.save(ROOT/(name+'.png'))
