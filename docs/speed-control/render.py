"""Render the actual bench records and explicitly labelled nominal models."""
from pathlib import Path
import hashlib,json,re,shutil
import numpy as np
import pandas as pd
from scipy import signal
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

HERE=Path(__file__).parent;ROOT=HERE.parents[1]
OLD=ROOT/'build/bench_debug/20261003_030219'
SWEEP=ROOT/'build/bench_debug/20261003_040347'
NEW=ROOT/'build/bench_debug/20261003_044515'
ASSETS=HERE/'figures';DATA=HERE/'data'
ASSETS.mkdir(parents=True,exist_ok=True);DATA.mkdir(exist_ok=True)
plt.rcParams.update({'font.family':'sans-serif','font.sans-serif':['Microsoft YaHei','DejaVu Sans'],
 'font.size':10,'axes.titlesize':12,'axes.labelsize':10,'axes.spines.top':False,
 'axes.spines.right':False,'axes.grid':True,'grid.alpha':.17,'grid.linewidth':.6,
 'svg.fonttype':'none','pdf.fonttype':42,'axes.unicode_minus':False,'figure.dpi':130})
BEFORE='#ba643b';NOW='#176481';MID='#709674';GRAY='#7b8287'
manifest=[];metrics={};sources={}
DTYPE=np.dtype([('us','<u4'),('x','<f4',(6,))])
def source(path):
 path=Path(path);key=str(path.relative_to(ROOT));sources[key]={'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()};return path
def csv(path):return pd.read_csv(source(path))
def native(path):
 d=np.fromfile(source(path),DTYPE);t=np.r_[0,np.cumsum(np.diff(d['us'].astype(np.int64))&0xffffff)]*1e-6
 return t,d['x'].astype(float)
def save(name,fig,title,kind,caption):
 for ext in ('svg','png','pdf'):fig.savefig(ASSETS/f'{name}.{ext}',bbox_inches='tight',dpi=210)
 plt.close(fig);manifest.append(dict(name=name,title=title,kind=kind,caption=caption))
def export(name,columns):pd.DataFrame(columns).to_csv(DATA/f'{name}.csv',index=False,float_format='%.10g')
def legend(ax):ax.legend(frameon=False,fontsize=9)
def phase(df,name):
 d=df[(df.phase==name)&(df.state==4)&(df.fault==0)].copy();d['t']=d.host_rx_s-d.host_rx_s.iloc[0];return d
def steady(d):return d[d.t>=2]
def raw_speed(angle,fs=500):return signal.savgol_filter(np.rad2deg(np.unwrap(np.deg2rad(angle))),25 if fs==500 else 21,3,deriv=1,delta=1/fs)/6
def frf(u,y,fs,velocity=False):
 f,pu=signal.welch(u,fs,nperseg=1024,detrend='linear');_,cross=signal.csd(u,y,fs,nperseg=1024,detrend='linear');_,coh=signal.coherence(u,y,fs,nperseg=1024,detrend='linear')
 h=cross/np.maximum(pu,1e-30)
 if velocity:h*=2j*np.pi*f/6
 return f,h,coh

# The algorithm flow is drawn as vector artwork, including the actual feedback.
fig,axs=plt.subplots(1,2,figsize=(12.8,10.2),layout='constrained')
flows=[('修改前 / bcb41e3',BEFORE,[
 'B/C ADC 与编码器采样\n有效性与保护检查 · 20 kHz',
 '同一角度修正 + 累计位置\nθ = θraw + 0.52° cos(2θraw)',
 '角度差分求速度\nΔθ × 20000 / 6',
 '单极点速度平滑\nα = 0.01 · 等效约 31.99 Hz',
 '速度目标 / 位置 P 生成速度目标\n外环每约 1 ms 更新',
 '原位置式 PI\nKp = 0.1 · Ki = 2',
 'Iq 限幅 + 原积分反算\n积分状态另限幅 ±Imax',
 'd/q 电流 PI 与解耦\n20 kHz · 原有电压抗饱和',
 'PWM 提交 → 电机\n窗口、相电流及编码器保护']),
 ('当前 / 2dc6b77',NOW,[
 'B/C ADC 与编码器采样\n有效性与保护检查 · 20 kHz',
 '同一角度修正 + 累计位置\n原始绝对角度另供前馈查表',
 '三状态预测：角度 / 速度 / 负载\n实际 Iq → 预测机械加速度',
 '用角度残差修正三个状态\n20 kHz · 修正极点 −25 rad/s',
 '速度目标 / 原位置 P\n外环每约 1 ms 更新',
 '增量二自由度 PI · β = 0.8\nKp = 0.1 · Ki = 2 保持不变',
 '加 512 点角度前馈后统一限幅\n用最终限幅结果反算 PI 状态',
 '同一 d/q 电流 PI 与解耦\n20 kHz · 电流环算法未改变',
 '同一 PWM 提交 → 电机\n全部保护保持有效'])]
for ax,(title,color,labels) in zip(axs,flows):
 ax.set(xlim=(0,10),ylim=(0,11.2));ax.axis('off');ax.set_title(title,color=color,fontweight='bold',pad=18)
 for i,label in enumerate(labels):
  y=10.1-i*1.1;ax.add_patch(FancyBboxPatch((1.3,y-.4),7.1,.8,boxstyle='round,pad=.03,rounding_size=.05',fc='#fbfaf6',ec=color,lw=1));ax.text(4.85,y,label,ha='center',va='center',fontsize=10)
  if i<8:ax.add_patch(FancyArrowPatch((4.85,y-.42),(4.85,y-.68),arrowstyle='-|>',mutation_scale=11,color=color))
 ax.plot([8.5,9.2,9.2,8.5],[1.3,1.3,9,9],color=GRAY,lw=1)
 ax.annotate('',xy=(8.45,9),xytext=(9.2,9),arrowprops={'arrowstyle':'->','color':GRAY})
 ax.text(9.4,5.2,'编\n码\n器\n反\n馈',va='center',color=GRAY,fontsize=9)
 if color==NOW:
  ax.text(.65,6.9,'实际 Iq\n输入',rotation=90,ha='center',va='center',color=NOW,fontsize=9)
  ax.plot([1.2,.35,.35,1.2],[2.4,2.4,7.9,7.9],color=NOW,lw=1)
  ax.annotate('',xy=(1.25,7.9),xytext=(.35,7.9),arrowprops={'arrowstyle':'->','color':NOW})
save('01_flow',fig,'算法逐步流程：结构改变在哪里','代码核对','两边共用角度修正、ADC、d/q 电流 PI 与 PWM。当前速度反馈来自三状态观测器；原 foc.rpm 仍用于电角速度计算和独立超速保护。箭头为控制反馈关系，不代表新增中断。')

# Measured d/q responses: only corrected, repeated pulse acquisitions are used.
fig,axs=plt.subplots(1,2,figsize=(12.8,4.3),layout='constrained')
for ax,axis in zip(axs,('d','q')):
 for variant,label,color in [('before','原结构',BEFORE),('now_fixed','当前结构',NOW)]:
  t,x=native(NEW/f'{variant}_{axis}_step.bin');cycles=x.reshape(16,256,6);j=0 if axis=='d' else 2
  y=cycles[:,:,j+1];u=cycles[:,:,j];tt=(np.arange(256)-128)*.05;mean=y.mean(axis=0);sd=y.std(axis=0)
  if axis=='q':mean-=.01;u=u-.01
  ax.plot(tt,mean,color=color,label=label,lw=1.7);ax.fill_between(tt,mean-sd,mean+sd,color=color,alpha=.13,lw=0)
  export(f'{variant}_{axis}_step',{'time_ms':tt,'reference_A':u.mean(axis=0),'current_mean_A':mean,'across_pulse_std_A':sd})
 ax.step(tt,u.mean(axis=0),color=GRAY,where='post',ls='--',label='目标增量 0.2 A');ax.set(xlim=(-1,4),xlabel='相对上升沿时间 / ms',ylabel='电流增量 / A',title=f'{axis.upper()} 轴：16 个小幅脉冲对齐');legend(ax)
save('02_current_step',fig,'实测电流闭环：前后没有改变电流控制器','实测 · 20 kHz','每个脉冲周期 12.8 ms，通电段 6.4 ms。粗线为 16 次脉冲均值，色带为跨脉冲 ±1 标准差，不是目标电流验收时的额外滤波。q 轴扣去已知 0.01 A 工作点，仅用于展示 0.2 A 阶跃增量。无效的首批 q 激励数据完全排除。')

fig,axs=plt.subplots(3,2,figsize=(12.8,8.3),layout='constrained')
for col,axis in enumerate(('d','q')):
 for variant,label,color in [('before','原结构',BEFORE),('now_fixed','当前结构',NOW)]:
  t,x=native(NEW/f'{variant}_{axis}_prbs.bin');j=0 if axis=='d' else 2;f,h,c=frf(x[:,j],x[:,j+1],20000);keep=(f>0)&(f<=2500);good=keep&(c>=.6)
  axs[0,col].semilogx(f[good],20*np.log10(abs(h[good])),'.-',ms=3,lw=1,color=color,label=label)
  axs[1,col].semilogx(f[good],np.angle(h[good],deg=True),'.-',ms=3,lw=1,color=color)
  axs[2,col].semilogx(f[keep],c[keep],color=color,lw=1)
  export(f'{variant}_{axis}_frf',{'frequency_Hz':f,'gain_real':h.real,'gain_imag':h.imag,'coherence':c})
 f=np.geomspace(10,2500,600);h=1/(1+1j*f/600);axs[0,col].semilogx(f,20*np.log10(abs(h)),':',color=GRAY,label='理想 600 Hz 模型');axs[1,col].semilogx(f,np.angle(h,deg=True),':',color=GRAY)
 axs[0,col].set_title(f'{axis.upper()} 轴目标 → 同轴测得电流');axs[2,col].axhline(.6,color=GRAY,ls='--',lw=.8);axs[2,col].set(xlabel='频率 / Hz',ylim=(0,1.03));legend(axs[0,col])
for ax,label in zip(axs[:,0],('幅值 / dB','相位 / °','相干性 γ²')):ax.set_ylabel(label)
save('03_current_frf',fig,'实测电流闭环 Bode 与相干性','实测 · 小幅 PRBS','±0.2 A、5 kHz PRBS 更新，20 kHz 原生采样 4096 点；Welch/CSD 1024 点、50% 重叠、频率间隔 19.53 Hz。幅相只显示相干性 ≥0.6 的点，相干性图保留全部频点。曲线为原始幅比，未强制归一化为 0 dB。该小幅工作点与理想 RL 模型有明显差异，不能由此断言真实电流带宽恰为 600 Hz。')

# Paired speed/position transient records, time-stamped by the MCU.
responses={}
for variant in ('before','now'):
 t,x=native(NEW/f'{variant}_speed_response.bin');v=raw_speed(x[:,0],1000);edges=np.flatnonzero(np.diff(x[:,4])<-1000)+1;reverse=int(edges[0]);responses[variant]=(t,x,v,reverse)
for event,startlevel,endlevel,name in [('step',0,1000,'04_speed_step'),('reverse',1000,-1000,'05_speed_reverse')]:
 fig,axs=plt.subplots(3,1,figsize=(12.8,8),sharex=True,layout='constrained');stats={}
 for variant,label,color in [('before','原结构',BEFORE),('now','当前结构',NOW)]:
  t,x,v,edge=responses[variant];begin=0 if event=='step' else edge;end=edge-12 if event=='step' else len(t)-12;tt=t[begin:end]-t[begin]
  axs[0].plot(tt,v[begin:end],color=color,lw=1.15,label=f'{label} · 相同角度导数');axs[1].plot(tt,x[begin:end,3],color=color,lw=1.15,label=f'{label} · 控制器速度反馈');axs[2].plot(tt,x[begin:end,1],color=color,lw=1.15,label=label)
  progress=(v[begin:end]-startlevel)/(endlevel-startlevel)
  def crossing(level):
   k=np.flatnonzero(progress>=level);return float(tt[k[0]]) if len(k) else None
  stats[variant]={'t10_s':crossing(.1),'t90_s':crossing(.9),'cross50_s':crossing(.5),'overshoot_rpm':float(np.max((v[begin:end]-endlevel)*np.sign(endlevel-startlevel))),'iq_target_std_after_2s_A':float(np.std(x[begin:end,1][tt>=2])) if max(tt)>2 else None}
  export(f'{variant}_{event}',{'mcu_time_s':tt,'raw_angle_derived_rpm':v[begin:end],'control_feedback_rpm':x[begin:end,3],'iq_target_A':x[begin:end,1],'reference_rpm':x[begin:end,4]})
 for ax in axs[:2]:ax.axhline(endlevel,color=GRAY,ls='--',lw=.8);ax.set_ylabel('速度 / rpm');legend(ax)
 axs[2].set(ylabel='完整 Iq 目标 / A',xlabel='MCU 相对时间 / s',xlim=(0,.5));legend(axs[2]);metrics[event]=stats
 save(name,fig,'实测 '+('0 → +1000 rpm 启动' if event=='step' else '+1000 → −1000 rpm 反转'),'实测 · MCU 时间戳','前后使用相同 2 A 目标上限。顶部速度统一由原始角度作 1 kHz/21 点三阶 SG 导数，用于相同口径比较，仍受编码器周期误差影响；中部显示各控制器自己的反馈，不能用它替代真实运动证据。底部为完整未滤波的电流目标。')

fig,axs=plt.subplots(2,1,figsize=(12.8,6.6),sharex=True,layout='constrained')
for variant,label,color in [('before','原结构',BEFORE),('now','当前结构',NOW)]:
 t,x=native(NEW/f'{variant}_position_step.bin');theta=x[:,0]+.52*np.cos(np.deg2rad(2*x[:,0]));pos=np.rad2deg(np.unwrap(np.deg2rad(theta)));pos-=pos[0]
 axs[0].plot(t,pos,color=color,label=label);axs[1].plot(t,x[:,1],color=color,label=label);export(f'{variant}_position_step',{'mcu_time_s':t,'corrected_encoder_travel_deg':pos,'iq_target_A':x[:,1]})
axs[0].axhline(1,color=GRAY,ls='--',label='目标 1°');axs[0].set(ylabel='编码器角度行程 / °',title='1° 位置小阶跃，位置 P=4 rpm/° 保持不变');axs[1].set(xlabel='MCU 相对时间 / s',ylabel='完整 Iq 目标 / A',xlim=(0,1));legend(axs[0]);legend(axs[1])
save('06_position_step',fig,'实测位置闭环：同一位置 P，不同速度反馈','实测 · MCU 时间戳','位置输出由原始绝对角度加同一已有 0.52° 修正后求行程，不使用速度观测器积分。零位仅定义本次相对行程；不改变前馈表的绝对物理索引。该工作点有摩擦、前馈和噪声，不能等同于全工况线性位置模型。')

for mode,name,title in [('speed','07_speed_frf','实测速度闭环 Bode'),('position','08_position_frf','实测位置闭环 Bode')]:
 fig,axs=plt.subplots(3,1,figsize=(12.8,8),sharex=True,layout='constrained')
 for variant,label,color in [('before','原结构',BEFORE),('now','当前结构',NOW)]:
  t,x=native(NEW/f'{variant}_{mode}_prbs.bin');theta=x[:,0]
  if mode=='position':theta=theta+.52*np.cos(np.deg2rad(2*theta))
  theta=np.rad2deg(np.unwrap(np.deg2rad(theta)));f,h,c=frf(x[:,4],theta,1000,mode=='speed');keep=(f>=2)&(f<=100 if mode=='speed' else f<=30);good=keep&(c>=.6)
  axs[0].semilogx(f[good],20*np.log10(abs(h[good])),'.-',ms=4,color=color,label=label);axs[1].semilogx(f[good],np.angle(h[good],deg=True),'.-',ms=4,color=color);axs[2].semilogx(f[keep],c[keep],color=color)
  export(f'{variant}_{mode}_frf',{'frequency_Hz':f,'gain_real':h.real,'gain_imag':h.imag,'coherence':c})
  metrics[f'{variant}_{mode}_frf']={'reliable_points':int(sum(good)),'total_points':int(sum(keep)),'min_coherence':float(c[keep].min())}
 axs[0].set_ylabel('闭环幅值 / dB');axs[1].set_ylabel('相位 / °');axs[2].set(xlabel='频率 / Hz',ylabel='相干性 γ²',ylim=(0,1.03));axs[2].axhline(.6,color=GRAY,ls='--',lw=.8);legend(axs[0])
 save(name,fig,title,'实测 · 小幅 PRBS','1 kHz 原生采样 4096 点，1024 点 Welch/CSD，间隔 0.977 Hz。速度在 300 rpm 工作点加 ±5 rpm、约 250 Hz 更新的 PRBS；输出用角度频谱乘 jω/6 得到，不使用观测器输出，不增加离线测速低通。位置加 ±0.25° PRBS，输出采用同一修正后的编码器角度。仅显示相干性 ≥0.6 的幅相点；短记录、周期误差和非线性限制了可信频带，没有把空缺连成“实测曲线”。')

# Existing high-speed captures plus this round's optional full speed coverage.
oldhigh=csv(OLD/'baseline_high.csv');newhigh=csv(NEW/'now_high.csv') if (NEW/'now_high.csv').exists() else csv(SWEEP/'high.csv')
new_prefix='high_' if (NEW/'now_high.csv').exists() else 'high_';summary=[]
fig,axs=plt.subplots(2,2,figsize=(12.8,6.7),layout='constrained')
for col,rpm in enumerate((1000,3000)):
 for df,ph,label,color in [(oldhigh,f'hold_{rpm}','原高增益 PI',BEFORE),(newhigh,f'high_{rpm}' if (NEW/'now_high.csv').exists() else f'high_{rpm}_0','当前观测器 + 2DOF PI',NOW)]:
  if ph not in df.phase.values:continue
  d=steady(phase(df,ph));w=d[(d.t>=3)&(d.t<4)];tt=w.t-3
  axs[0,col].plot(tt,w.iq_target_A,lw=1,color=color,label=label);axs[1,col].plot(tt,w.rpm,lw=1,color=color,label=label)
 axs[0,col].set(title=f'+{rpm} rpm 稳态',ylabel='完整 Iq 目标 / A');axs[1,col].set(xlabel='记录内相对时间 / s',ylabel='各自控制反馈 / rpm');legend(axs[0,col])
save('09_high_trace',fig,'实测高速：电流目标与控制器反馈','实测 · 500 Hz','图中各取固定 3～4 秒作可读波形；统计使用所有故障前稳态数据、排除前 2 秒，绝不只统计这一小段。底部是各自控制器反馈，滤波/观测方法不同，其标准差不能直接作为真实机械平滑度证明。')
for rpm in (1000,3000,5000,-1000,-3000,-5000):
 for df,ph,label in [(oldhigh,f'hold_{rpm}','before'),(newhigh,f'high_{rpm}' if (NEW/'now_high.csv').exists() else f'high_{rpm}_0','now')]:
  if ph not in df.phase.values:continue
  d=steady(phase(df,ph))
  if len(d)<5000:continue
  angle=np.rad2deg(np.unwrap(np.deg2rad(d.encoder_deg)));row={'rpm':rpm,'version':label,'samples':len(d),'duration_s':float(d.t.iloc[-1]-d.t.iloc[0]),'iq_target_std_A':float(d.iq_target_A.std(ddof=0)),'id_rms_A':float(np.sqrt(np.mean(d.id_A**2))),'iq_rms_A':float(np.sqrt(np.mean(d.iq_A**2))),'raw_mean_rpm':float((angle[-1]-angle[0])/6/(d.host_rx_s.iloc[-1]-d.host_rx_s.iloc[0])),'opposing_fraction':float(np.mean(d.iq_target_A*d.rpm<0))};summary.append(row)
metrics['high']=summary
for r in summary:
 if r['version']=='now':
  t,x=native(NEW/f'now_high_high_{r["rpm"]}.bin');r['native_iq_target_std_A']=float(x[:,1].std())
fig,ax=plt.subplots(figsize=(12.8,4.8),layout='constrained');rpms=(1000,3000,5000,-1000,-3000,-5000);xx=np.arange(6)
for version,label,color,offset in [('before','原高增益 PI',BEFORE,-.19),('now','当前参数',NOW,.19)]:
 vals=[next((r['iq_target_std_A'] for r in summary if r['version']==version and r['rpm']==rpm),np.nan) for rpm in rpms];bars=ax.bar(xx+offset,vals,.35,color=color,label=label)
 for b,v in zip(bars,vals):
  if np.isfinite(v):ax.text(b.get_x()+b.get_width()/2,v*1.12,f'{v:.4f}',ha='center',fontsize=8)
ax.axhline(.02,color='#7b4545',ls='--',lw=1,label='验收线 0.02 A');ax.set(yscale='log',ylim=(.002,2),xticks=xx,xticklabels=[f'{v:+d}' for v in rpms],xlabel='目标速度 / rpm',ylabel='完整 Iq 目标标准差 / A');legend(ax)
save('10_high_stats',fig,'高速稳态统计：不能只看曲线是否平滑','实测 · 未滤波统计','每段保留超过 10 秒稳态；空缺表示没有合格长度的数据，不是零波动。跨轮对照保持相同控制增益和 2 A 目标上限，电池电压与时间不同。当前仅单次各工况，不代替三次重复验收，也不代表温升、声压已经测量。')

sweep=csv(SWEEP/'sweep.csv');fig,axs=plt.subplots(1,2,figsize=(12.8,4.5),layout='constrained');ss=[]
for pole in (200,80,40,25):
 d=steady(phase(sweep,f'w{pole}_1000'));t,x=native(SWEEP/f'w{pole}_native_1000.bin');q=float(d.iq_target_A.std(ddof=0));ss.append(dict(pole=pole,telemetry_std_A=q,native_std_A=float(x[:,1].std())))
 d=d[(d.t>=3)&(d.t<3.2)];axs[0].plot(d.t-3,d.iq_target_A,label=f'{pole} rad/s',lw=1)
axs[1].plot([r['pole'] for r in ss],[r['telemetry_std_A'] for r in ss],'o-',color=NOW,label='500 Hz 长记录');axs[1].plot([r['pole'] for r in ss],[r['native_std_A'] for r in ss],'s--',color=BEFORE,label='20 kHz 原生短记录');axs[1].axhline(.02,ls=':',color=GRAY);axs[1].set(xscale='log',yscale='log',xlabel='修正极点绝对值 / rad/s',ylabel='完整 Iq 目标标准差 / A');axs[0].set(xlabel='稳态窗口时间 / s',ylabel='完整 Iq 目标 / A');legend(axs[0]);legend(axs[1]);metrics['sweep']=ss
save('11_observer_sweep',fig,'只改观测器修正增益的同轮对照','实测 · +1000 rpm','速度 Kp/Ki、β、前馈表、角度修正保持不变。图证明该观测器增益与高速电流目标波动相关；它不能单独证明编码器误差、真实转矩扰动和闭环振荡各占多少。')

oldlow=csv(NEW/'before_low.csv') if (NEW/'before_low.csv').exists() else csv(OLD/'baseline_low.csv')
newlow=csv(NEW/'now_low.csv') if (NEW/'now_low.csv').exists() else csv(NEW/'now.csv')
fig,axs=plt.subplots(3,1,figsize=(12.8,9),layout='constrained');lowstats=[]
for df,ph,label,color in [(oldlow,'low_1' if (NEW/'before_low.csv').exists() else 'cog_1','原结构',BEFORE),(newlow,'low_1','当前参数',NOW)]:
 d=steady(phase(df,ph));a=np.rad2deg(np.unwrap(np.deg2rad(d.encoder_deg)));tt=np.arange(len(d))/500;v=raw_speed(d.encoder_deg.to_numpy());iq=d.iq_target_A.to_numpy();est=d.rpm.to_numpy()
 axs[0].plot(tt[::10],(a-a[0])[::10],color=color,label=label);axs[1].plot(tt,v,color=color,label=label+' · 同口径原始角度测速',lw=.7);axs[2].plot(tt,est,color=color,ls='--',label=label+' · 控制反馈',lw=.7)
 row=dict(version='before' if color==BEFORE else 'now',direction=1,travel_deg=float(a[-1]-a[0]),elapsed_s=float(d.host_rx_s.iloc[-1]-d.host_rx_s.iloc[0]),raw_mean_rpm=float((a[-1]-a[0])/6/(d.host_rx_s.iloc[-1]-d.host_rx_s.iloc[0])),raw_speed_std_rpm=float(v[25:-25].std()),estimated_mean_rpm=float(est.mean()),iq_target_std_A=float(iq.std()));lowstats.append(row)
 export(f'low_{row["version"]}',{'relative_time_s':tt,'raw_travel_deg':a-a[0],'raw_angle_derived_rpm':v,'controller_rpm':est,'iq_target_A':iq})
axs[0].set(xlabel='稳态记录时间 / s',ylabel='原始角度累计行程 / °',title='+1 rpm：应按 6°/s 连续行进');axs[1].axhline(1,color=GRAY,ls=':');axs[1].set(xlabel='完整稳态记录时间 / s',ylabel='同口径角度测速 / rpm');axs[2].set(xlabel='完整稳态记录时间 / s',ylabel='控制器反馈 / rpm');legend(axs[0]);legend(axs[1]);legend(axs[2]);metrics['low']=lowstats
for rpm in (-1,5,-5,50,-50):
 for df,ph,version in [(oldlow,'low_-1','before'),(newlow,'low_-1','now')] if rpm==-1 else [(csv(OLD/'baseline_low.csv'),f'low_{rpm}','before'),(csv(NEW/'now.csv'),f'low_{rpm}','now')]:
  d=steady(phase(df,ph));a=np.rad2deg(np.unwrap(np.deg2rad(d.encoder_deg)));v=raw_speed(d.encoder_deg.to_numpy());duration=float(d.host_rx_s.iloc[-1]-d.host_rx_s.iloc[0]);lowstats.append(dict(version=version,direction=rpm,travel_deg=float(a[-1]-a[0]),elapsed_s=duration,raw_mean_rpm=float((a[-1]-a[0])/6/duration),raw_speed_std_rpm=float(v[25:-25].std()),estimated_mean_rpm=float(d.rpm.mean()),iq_target_std_A=float(d.iq_target_A.std(ddof=0))))
save('12_low_motion',fig,'低速关键反证：显示速度平稳，不等于轴连续转动','实测 · 同一种角度处理','完整稳态角度行程、同口径角度测速与控制反馈并列，保留全部角度测速波形。两版本统一使用 500 Hz/25 点三阶 SG 导数，只处理角度，未处理目标电流。当前低速停顿/快速位移仍明显；必须将这项退化与高速电流改善一起评价。')

# Cogging table: measured, repeatable angular requests, not claimed pure cogging.
table=np.array([float(v) for v in re.findall(r'([-+]?\d+\.\d+)f',(ROOT/'App/Control/cogging_table.inc').read_text())]);assert len(table)==512
train=csv(OLD/'baseline_low.csv');fig,axs=plt.subplots(1,2,figsize=(12.8,4.7),layout='constrained')
for direction,color in [(1,BEFORE),(-1,NOW)]:
 d=phase(train,f'cog_{direction}');ang=d.encoder_deg.to_numpy();q=d.iq_target_A.to_numpy();travel=direction*np.rad2deg(np.unwrap(np.deg2rad(ang)));travel-=travel[0]+6;turn=np.floor(travel/360).astype(int);bins=(ang*512/360).astype(int)&511;waves=[]
 for k in range(4):
  mask=turn==k;count=np.bincount(bins[mask],minlength=512);wave=np.bincount(bins[mask],weights=q[mask],minlength=512)/count;waves.append(wave-wave.mean())
 axs[0].plot(np.arange(512)*360/512,waves[3],color=color,alpha=.7,label=f'{direction:+d} rpm 第四圈验证')
 x=ang[turn==3]*512/360;i=x.astype(int)&511;lookup=table[i]+(x-i)*(table[(i+1)&511]-table[i]);res=q[turn==3]-q[turn==3].mean()-lookup
 axs[1].plot(np.arange(512)*360/512,waves[3]-table,color=color,lw=.8,label=f'{direction:+d} rpm 角度分箱残差');metrics[f'cog_validation_{direction}']=float(np.sqrt(np.mean(res**2)))
axs[0].plot(np.arange(512)*360/512,table,color='#303d46',lw=1.1,label='编译进固件的表');axs[0].set(xlabel='原始绝对机械角度 / °',ylabel='去直流电流周期量 / A');axs[1].set(xlabel='机械角度 / °',ylabel='第四圈减查表 / A');legend(axs[0]);legend(axs[1])
save('13_cog_table',fig,'前馈表的来源、跨方向重复性与独立第四圈','离线标定验证','正反方向前三圈标定，第四圈独立验证；去直流、保留跨圈/方向可重复角度阶次，512 点线性插值。角度分箱验证与全部原始采样插值验证是两个口径；后者残差约 0.0186/0.0176 A。低速新的运动结果说明“表能解释电流周期量”不等于“闭环已经改善”。')

fig,axs=plt.subplots(1,2,figsize=(12.8,4.7),layout='constrained')
for rpm,color in [(1000,BEFORE),(3000,NOW),(5000,MID)]:
 t,x=native(OLD/f'native_{rpm}.bin');a=np.rad2deg(np.unwrap(np.deg2rad(x[:,0])));a-=np.polyval(np.polyfit(t,a,1),t);f,ps=signal.periodogram(a,20000);keep=(f>=20)&(f<=4000);axs[0].semilogy(f[keep],ps[keep],color=color,lw=.9,label=f'{rpm} rpm')
 true=36*rpm/60;alias=abs((true+250)%500-250);axs[1].plot([rpm],[true],'o',color=color);axs[1].plot([rpm],[alias],'x',color=color,ms=9)
axs[0].set(xlabel='原生频率 / Hz',ylabel='角度残差 PSD / °²/Hz',title='20 kHz 原生角度记录');axs[1].plot([1000,3000,5000],[600,1800,3000],color=GRAY,label='36 机械阶次');axs[1].plot([1000,3000,5000],[100,200,0],ls='--',color=GRAY,label='500 Hz 采样折叠位置');axs[1].set(xlabel='速度 / rpm',ylabel='频率 / Hz',title='折叠例子：同一角度周期项');legend(axs[0]);legend(axs[1])
save('14_aliasing',fig,'为什么 500 Hz 日志中的峰不能直接用来设计陷波','原生数据 + 采样关系','以原始角度减线性趋势后的 PSD 展示；约 36 机械阶次在原生记录中随转速移动。图中的折叠位置是采样数学关系，不代表该峰一定是机械共振；尚不能排除编码器非线性。')

# Nominal continuous frequency models: their limitations are printed in the page.
A=4685.833948;KP=.1;KI=2.;POS=4.;WCI=2*np.pi*600;WO=-np.log(.99)/.00005
f=np.geomspace(.02,10000,10000);s=2j*np.pi*f;P=A/s;Ti=1/(1+s/WCI);Do=np.exp(-s*.0005);C=KP+KI/s;Cr=.8*KP+KI/s;H=WO/(s+WO)
Lold=C*P*Ti*Do*H;Lnew=C*P*Ti*Do;Told=C*P*Ti*Do/(1+Lold);Tnew=Cr*P*Ti*Do/(1+Lnew)
def margins(freq,loop):
 mag=20*np.log10(abs(loop));ph=np.unwrap(np.angle(loop))*180/np.pi
 if ph[0]>0:ph-=360
 cross=np.flatnonzero(np.diff(mag>0));pcross=np.flatnonzero((ph[:-1]>-180)&(ph[1:]<=-180));out={}
 if len(cross):
  i=cross[0];fc=np.exp(np.interp(0,mag[i:i+2][::-1],np.log(freq[i:i+2])[::-1]));out.update(crossover_Hz=float(fc),phase_margin_deg=float(180+np.interp(np.log(fc),np.log(freq),ph)))
 if len(pcross):
  i=pcross[0];ff=np.exp(np.interp(-180,ph[i:i+2][::-1],np.log(freq[i:i+2])[::-1]));out['gain_margin_dB']=float(-np.interp(np.log(ff),np.log(freq),mag))
 return out
metrics['nominal_margins']={'before':margins(f,Lold),'now':margins(f,Lnew)}
def bode(axs,curves,limit):
 for label,color,h in curves:
  keep=(f>=limit[0])&(f<=limit[1]);ph=np.unwrap(np.angle(h))*180/np.pi
  if ph[0]>0:ph-=360
  axs[0].semilogx(f[keep],20*np.log10(abs(h[keep])),color=color,label=label);axs[1].semilogx(f[keep],ph[keep],color=color)
 axs[0].axhline(0,ls=':',color=GRAY,lw=.8);axs[0].set_ylabel('幅值 / dB');axs[1].set(xlabel='频率 / Hz',ylabel='相位 / °');legend(axs[0])
for which,name,title in [('current','15_model_current','电流环名义模型'),('speed','16_model_speed','速度环名义开环与闭环'),('position','17_model_position','位置环名义开环与闭环')]:
 fig,axs=plt.subplots(2,2,figsize=(12.8,7.3),layout='constrained')
 if which=='current':
  li=WCI/s;tc=li/(1+li);bode(axs[:,0],[('前后同一电流环',NOW,li)],(1,10000));bode(axs[:,1],[('前后同一闭环',NOW,tc)],(1,10000));axs[0,0].set_title('开环 Ci(s) × 1/(Ls+R)');axs[0,1].set_title('闭环目标 → 测量电流')
 elif which=='speed':
  lowc=.005+.01/s;lowL=lowc*P*Ti*Do*H;lowT=lowc*P*Ti*Do/(1+lowL);bode(axs[:,0],[('更早低增益 0.005/0.01',GRAY,lowL),('原高增益 + 原测速',BEFORE,Lold),('当前结构 · 理想模型匹配',NOW,Lnew)],(.1,500));bode(axs[:,1],[('更早低增益',GRAY,lowT),('原高增益',BEFORE,Told),('当前 β=0.8',NOW,Tnew)],(.1,500));axs[0,0].set_title('名义开环 · 0 dB 交越与相位');axs[0,1].set_title('真实速度对参考的名义闭环')
 else:
  po=6*POS/s*Told;pn=6*POS/s*Tnew;bode(axs[:,0],[('原结构',BEFORE,po),('当前结构',NOW,pn)],(.02,100));bode(axs[:,1],[('原结构',BEFORE,po/(1+po)),('当前结构',NOW,pn/(1+pn))],(.02,100));axs[0,0].set_title('位置开环：6Kpos × T速度 / s');axs[0,1].set_title('位置参考 → 机械角度')
 save(name,fig,title,'名义模型 · 不是实测','电流：R=0.12 Ω、L=50 µH、Kp=0.1884956、Ki=452.3893，忽略数字延迟和耦合；速度：A=4685.83 rpm/s/A、600 Hz 一阶电流通路、0.5 ms 外环等效延迟；位置：Kpos=4 rpm/°，冻结前馈，忽略限幅。新观测器仅在机械模型完全匹配时对真实运动得到理想预测；本图没有把其角度修正当成串联测速低通。实测电流 FRF 与这些简化假设不同，因此不能把模型裕量写成硬件验收通过。')

fig,axs=plt.subplots(2,1,figsize=(12.8,6.6),sharex=True,layout='constrained')
for pole,color,label in [(200,MID,'首次观测器 200 rad/s'),(25,NOW,'当前观测器 25 rad/s')]:
 gu=A*s*(s+3*pole)/(s+pole)**3;gtheta=(.5*pole**2*s*s+pole**3*s/6)/(s+pole)**3;htheta=6*gtheta/s
 assert np.max(abs((gu+htheta*P)/P-1))<1e-10
 noise=-C*gtheta/(1+Lnew);keep=(f>=.1)&(f<=500);axs[0].semilogx(f[keep],20*np.log10(abs(noise[keep])),color=color,label=label);axs[1].semilogx(f[keep],np.angle(noise[keep],deg=True),color=color)
noise=-C*(s/6)*H/(1+Lold);keep=(f>=.1)&(f<=500);axs[0].semilogx(f[keep],20*np.log10(abs(noise[keep])),color=BEFORE,label='原角度差分 + 单极点');axs[0].set(ylabel='角度噪声 → Iq 目标 / dB(A/°)',title='相同速度 PI 增益下的角度噪声传播');axs[1].set(xlabel='噪声频率 / Hz',ylabel='相位 / °');legend(axs[0])
save('18_noise_path',fig,'降低的是角度噪声修正通路，不是速度 PI 增益','名义模型 · 噪声传递','输入定义为角度测量误差，单位 °；输出为最终 PI 电流目标，单位 A。模型不包含实际 Iq 测量噪声、角度误差的转矩耦合、查表项和量化，不能据此预测最终完整电流标准差。')

# Same-rate nominal simulation, without matrix libraries in firmware.
def simulate(kind,mode):
 dt=5e-5;duration=.5 if mode!='position' else 1.;v=theta=cur=feedback=esttheta=estv=load=state=lastp=q=0.;out=[]
 for n in range(round(duration/dt)):
  t=n*dt;dist=.02 if mode=='load' and t>=.05 else 0.;cur+=(1-np.exp(-WCI*dt))*(q-cur);v+=A*(cur-dist)*dt;theta+=6*v*dt
  if kind=='before':feedback+=.01*(v-feedback)
  else:
   w=kind;acc=A*(cur-load);esttheta+=6*dt*estv+3*dt*dt*acc;estv+=dt*acc;e=theta-esttheta;esttheta+=3*w*dt*e;estv+=.5*w*w*dt*e;load-=w**3/(6*A)*dt*e;feedback=estv
  if n%20==19:
   r=POS*(.1-theta) if mode=='position' else 0 if mode=='load' else 1.;err=r-feedback
   if kind=='before':wanted=KP*err+state;limited=np.clip(wanted,-8,8);state+=KI*err*.001+.1*(limited-wanted);state=np.clip(state,-8,8)
   else:
    p=.8*r-feedback;wanted=state+KP*(p-lastp)+KI*err*.001;lastp=p;limited=np.clip(wanted,-8,8);state=wanted+.02*(limited-wanted)
   q=limited;out.append((t,v,theta,q))
 return np.array(out)
fig,axs=plt.subplots(2,3,figsize=(12.8,7.2),layout='constrained')
for col,mode,title in [(0,'speed','1 rpm 小阶跃'),(1,'position','0.1° 位置小阶跃'),(2,'load','未知负载 +0.02 A 等效扰动')]:
 for kind,label,color in [('before','原高增益 PI',BEFORE),(200,'首次观测器 200',MID),(25,'当前观测器 25',NOW)]:
  d=simulate(kind,mode);axs[0,col].plot(d[:,0],d[:,2] if mode=='position' else d[:,1],color=color,label=label);axs[1,col].plot(d[:,0],d[:,3],color=color);export(f'model_{mode}_{kind}',{'time_s':d[:,0],'rpm':d[:,1],'position_deg':d[:,2],'iq_target_A':d[:,3]})
 axs[0,col].set(title=title,ylabel='位置 / °' if mode=='position' else '速度 / rpm');axs[1,col].set(xlabel='模拟时间 / s',ylabel='Iq 目标 / A');legend(axs[0,col])
save('19_model_time',fig,'同率模型时域：参考跟随与未知扰动不是同一个问题','算法模型仿真 · 不是实测','20 kHz 预测/电流通路、1 kHz 外环，600 Hz 标称一阶电流响应，同一机械 A；电流限制 8 A，小参考未触及限幅，前馈冻结为零，未加入编码器/ADC 噪声。保留源码的离散 PI 更新与抗饱和。降低观测器修正增益可能延迟未知负载识别，右列就是这一取舍；这不能代替摩擦、齿槽和不同惯量的实物验收。')

metrics['software']={'numpy':np.__version__,'pandas':pd.__version__,'scipy':__import__('scipy').__version__,'matplotlib':matplotlib.__version__}
(DATA/'metrics.json').write_text(json.dumps(metrics,indent=2,ensure_ascii=False),encoding='utf-8')
(DATA/'provenance.json').write_text(json.dumps(sources,indent=2,ensure_ascii=False),encoding='utf-8')
(HERE/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf-8')
print('Rendered',len(manifest),'vector figures; metrics and raw-source hashes saved.')
