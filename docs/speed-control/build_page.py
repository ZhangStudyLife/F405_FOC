"""Build an offline comparison page from rendered measurements and models."""
from pathlib import Path
import json,html,zipfile

P=Path(__file__).parent
M=json.loads((P/'data/metrics.json').read_text(encoding='utf-8'))
F=json.loads((P/'manifest.json').read_text(encoding='utf-8'))
S=json.loads((P/'data/provenance.json').read_text(encoding='utf-8'))
B=P.parents[1]/'build/bench_debug/20261003_044515'
final=json.loads((B/'final_verified.json').read_text())

def figure(number):
 f=F[number-1];name=f['name'];svg=(P/f'figures/{name}.svg').read_text(encoding='utf-8');svg=svg[svg.index('<svg'):]
 return f'''<figure id="fig-{number}"><div class="figure-head"><span>FIG. {number:02d}</span><span class="kind">{f['kind']}</span></div><button class="plot" aria-label="放大图 {number}：{f['title']}">{svg}</button><figcaption><strong>{f['title']}</strong><p>{f['caption']}</p><div class="exports">下载 <a href="figures/{name}.svg" download>SVG 矢量</a><a href="figures/{name}.pdf" download>PDF</a><a href="figures/{name}.png" download>PNG</a></div></figcaption></figure>'''

def table(headers,rows):
 return '<div class="table-scroll"><table><thead><tr>'+''.join(f'<th>{x}</th>' for x in headers)+'</tr></thead><tbody>'+''.join('<tr>'+''.join(f'<td>{x}</td>' for x in r)+'</tr>' for r in rows)+'</tbody></table></div>'

high=[]
for rpm in (1000,3000,5000,-1000,-3000,-5000):
 old=next(r for r in M['high'] if r['rpm']==rpm and r['version']=='before');new=next(r for r in M['high'] if r['rpm']==rpm and r['version']=='now')
 high.append([f'{rpm:+d}',f'{old["iq_target_std_A"]:.5f}',f'{new["iq_target_std_A"]:.5f}',f'{new["native_iq_target_std_A"]:.5f}',f'{new["raw_mean_rpm"]:.3f}',f'{new["iq_rms_A"]:.3f} / {new["id_rms_A"]:.3f}',f'{old["opposing_fraction"]*100:.1f}% → {new["opposing_fraction"]*100:.1f}%'])
low=[]
for rpm in (1,-1,5,-5,50,-50):
 old=next(r for r in M['low'] if r['direction']==rpm and r['version']=='before');new=next(r for r in M['low'] if r['direction']==rpm and r['version']=='now')
 low.append([f'{rpm:+d}',f'{old["raw_mean_rpm"]:.4f} → {new["raw_mean_rpm"]:.4f}',f'{old["raw_speed_std_rpm"]:.3f} → {new["raw_speed_std_rpm"]:.3f}',f'{old["iq_target_std_A"]:.4f} → {new["iq_target_std_A"]:.4f}',f'{new["estimated_mean_rpm"]:.4f}'])
transient=[]
for mode,label in [('step','0 → +1000 rpm'),('reverse','+1000 → −1000 rpm')]:
 old=M[mode]['before'];new=M[mode]['now'];a=old['t90_s']-old['t10_s'];b=new['t90_s']-new['t10_s']
 transient.append([label,f'{a*1000:.0f} → {b*1000:.0f} ms',f'{(b/a-1)*100:+.1f}%',f'{old["cross50_s"]*1000:.0f} → {new["cross50_s"]*1000:.0f} ms',f'{old["overshoot_rpm"]:.1f} → {new["overshoot_rpm"]:.1f} rpm'])

content=f'''
<section id="reading"><div class="section-no">01 / 阅读结论</div><h2>高速电流目标改善了，低速运动仍然退化。</h2>
<p class="lead">这份报告回答两件事：代码到底改了什么，以及电机实测是否支持这些改动。当前结构并未降低速度 PI 的 Kp/Ki；它改变了测速方法、参考响应、角度前馈和限幅后的状态恢复。</p>
<div class="findings"><div><span class="label">本轮高速 / 单次各点</span><strong>0.0085–0.0120 <small>A</small></strong><p>±1000、±3000、±5000 rpm 的完整 Iq 目标标准差。排除前 2 秒，保留约 11 秒原始数据，未减去前馈、未对 Iq 目标滤波。</p></div><div><span class="label">+1 rpm / 同口径角度测速</span><strong>0.22 → 5.73 <small>rpm</small></strong><p>低速速度波动显著增大。当前控制器显示的平均速度接近 1 rpm，原始角度却存在停顿与快速位移，低速验收失败。</p></div></div>
<div class="note bad"><strong>整体方案尚未通过验收。</strong>高速每点仅一次，未完成三次重复；低速已经发现退化；完整瞬态调节时间、补偿退出区间、300 秒波形未验收。声压、机械振动和温升没有对应仪器，均未测。以下“模型”不能用于替代这些实测结论。</div>
<p>本轮补采了电流 d/q 闭环小阶跃与 PRBS、速度与位置小信号闭环频响、启动与正反转、正负低速完整角度覆盖、六个高速点。这样可以同时看见“控制器的反馈变平滑”和“轴是否真的更平滑”的区别。电流环本身沿用原控制算法，外环才是本次结构调整的重点。</p>
<div class="legend"><span><i class="old"></i>修改前：原高增益 PI</span><span><i class="new"></i>当前：三状态观测器 + 二自由度 PI + 角度前馈</span><span>实测 / 代码核对 / 名义模型：各图单独标记</span></div>
</section>

<section id="versions"><div class="section-no">02 / 版本与参数</div><h2>先固定比较对象，避免把不同轮次当成同一次实验。</h2>
{table(['对象','代码依据','结构与参数','本报告用途'],[
 ['更早低增益','ad7a2e9；历史 24 通道日志','速度 Kp=0.005，Ki=0.01；原角度差分测速','名义模型中的第三条参考曲线；不冒充本轮实测'],
 ['修改前高增益','bcb41e3 的 control.c','Kp=0.1，Ki=2；差分 + α=0.01；位置式 PI；无本轮角度前馈','本轮电流/外环动态/±1 rpm 对照；前轮六点高速基线'],
 ['首次新结构','d8c1337','三状态角度/速度/负载观测器，修正极点 −200 rad/s；β=0.8；512 点前馈','同轮 200/80/40/25 rad/s 修正增益对照'],
 ['当前正式版本','2dc6b77eb21e07278975afbc61e9eefefbbec45b','同一新结构，修正极点降低至 −25 rad/s','本轮实际采样版本的控制逻辑；采集版暂设 2 A 限制'],
 ])}
<p>真实源码与历史参数给出的倍率是：比例增益从 0.005 到 0.1，为 <strong>20 倍</strong>；积分增益从 0.01 到 2，为 <strong>200 倍</strong>。之前提到的“30 倍”描述的是某轮目标电流波动之比，不能当成两项 PI 增益都扩大 30 倍。本轮高增益前后保持 0.1/2，故波动下降不能解释为简单降低 Kp。</p>
{table(['项目','修改前高增益','当前结构','是否变化'],[
 ['电流 / 外环频率','20 kHz / 约 1 kHz','20 kHz / 约 1 kHz','保持'],
 ['d/q 电流 Kp / Ki','0.1884956 / 452.3893','0.1884956 / 452.3893','保持，名义设计 600 Hz'],
 ['速度 Kp / Ki','0.1 A/rpm / 2 A/(rpm·s)','相同','保持；现有 set/save 路径'],
 ['位置 P / 速度限制','4 rpm/° / 100 rpm','相同','保持'],
 ['速度反馈','20 kHz 角度差分，α=0.01','20 kHz 三状态观测器','变化'],
 ['参考比例权重 β','1','0.8','只改变参考的比例通路'],
 ['机械加速度参数 A','未用于测速','4685.833948 rpm/s/A','本机前轮小扰动辨识值'],
 ['角度残差修正增益','无','Lθ=75；Lv=312.5；Lℓ=−0.555753084','对应连续修正极点 −25 rad/s'],
 ['角度前馈','无','512 个 float，线性插值；≤50 rpm 全量，≥200 rpm 退出','退出区间仍待开关对照'],
 ['机械角度修正','θraw + 0.52°cos(2θraw)','相同','本轮没有重标或改动'],
 ['Iq 目标限制','正式 8 A；对照采样 2 A','正式 8 A；对照采样 2 A','运行对照都用 2 A'],
 ['零偏判据 / 串口','1.4～1.9 V、原始标准差 ≤4 mV；20 通道','相同；500 Hz，84 B/帧，1 Mbaud','保持'],
 ])}
<p>前轮高速基线与本轮结果来自同一台架和相同高增益，但并非随机交错的重复实验，电池电压和温度条件也并未完全锁定。报告保留这种限制，不将跨轮差异全归因于单个算法。</p>
</section>

<section id="flow"><div class="section-no">03 / 前后算法流程</div><h2>保留电流内环，重新组织外环的反馈与目标生成。</h2>
{figure(1)}
<p>图表示控制的因果结构。实际原生调用先计算本拍电流控制与 PWM，再由 foc_outer_step 更新外环目标，供下一拍电流控制使用；图不是中断执行顺序图。</p><h3>修改前：噪声直接经过角度差分，再被高 Kp 放大。</h3>
<p>原始角度通过已有周期修正后，累积成多圈位置。20 kHz 相邻角度差求速度，再用 α=0.01 的单极点平滑；其连续等效极点约 201 rad/s，即 31.99 Hz。外环约每 1 ms 取一次这个反馈，速度模式直接使用 rpm 目标，位置模式先由位置 P 产生速度目标。</p>
<div class="equation">v<sub>raw</sub>[n] = Δθ[n] / (6T<sub>s</sub>)<br>v<sub>fb</sub>[n] = v<sub>fb</sub>[n−1] + 0.01(v<sub>raw</sub>[n] − v<sub>fb</sub>[n−1])<br>u = K<sub>p</sub>(r − v<sub>fb</sub>) + z； Iq* = clip(u, ±I<sub>max</sub>)<br>z ← clip[z + K<sub>i</sub>eΔt + 0.1(Iq* − u), ±I<sub>max</sub>]</div>
<p>只要反馈速度含有几 rpm 的纹波，0.1 A/rpm 的比例通路就会生成数百 mA 的目标电流纹波。这个关系说明高增益反馈为何会放大测速波动，却不能单凭日志区分真实运动扰动、编码器周期误差和闭环振荡。</p>
<h3>现在：用实际电流预测运动，再由角度残差修正。</h3>
<p>当前正式实现是<strong>固定增益的角度、速度、等效负载三状态观测器</strong>。没有在最终固件中留下两状态 PLL，也没有使用 Kalman 矩阵、在线协方差调整或自适应参数。实际 Iq 用于预测加速度；编码器角度只通过残差修正状态，避免把每个角度采样的差分噪声直接送入速度 PI。</p>
<div class="equation">â = A(Iq<sub>measured</sub> − ℓ̂)<br>θ̂<sup>−</sup> = θ̂ + 6T<sub>s</sub>v̂ + 3T<sub>s</sub>²â；v̂<sup>−</sup> = v̂ + T<sub>s</sub>â<br>ε = wrap<sub>±180°</sub>(θ<sub>corrected</sub> − θ̂<sup>−</sup>)<br>θ̂ ← θ̂<sup>−</sup> + L<sub>θ</sub>T<sub>s</sub>ε；v̂ ← v̂<sup>−</sup> + L<sub>v</sub>T<sub>s</sub>ε；ℓ̂ ← ℓ̂ + L<sub>ℓ</sub>T<sub>s</sub>ε</div>
<p>这里 θ 用度、v 用 rpm，ℓ̂ 用等效电流 A；方向按照现有校准方向统一。Lθ=3w，Lv=w²/2，Lℓ=−w³/(6A)，w=25 rad/s。20 kHz 路径只包含这些状态运算、角度保护和后续电流控制，频谱分析在主机离线完成。</p>
<p>这一结构参考了带绝对编码器 PMSM 的转矩输入/负载估计思路，但本工程的固定增益实现并不等于论文的自适应 Kalman 算法。<a href="https://ietresearch.onlinelibrary.wiley.com/doi/10.1049/iet-epa.2014.0516" target="_blank" rel="noreferrer">Xia 等的原论文</a>讨论测速噪声、二自由度控制和估计结构。最初评估的 <a href="https://github.com/mjbots/moteus/blob/8f747f4ac448d117c1390b77ca2d80c27e751a56/fw/motor_position.h" target="_blank" rel="noreferrer">moteus 编码器观测实现</a>提供工程参考，不能用来声明本机采用了相同估计器。</p>
<div class="note"><strong>25 rad/s 不是整个速度闭环只有 4 Hz。</strong>修正增益决定角度残差对状态的修正速度；模型中的实际电流输入仍预测加速度。降低修正增益可以抑制角度噪声，同时可能降低未知负载与模型误差的恢复能力。本轮低速退化必须正视，不能用“观测器输出更平滑”掩盖。</div>
<p>原 foc.rpm 测速仍供电角速度前馈和独立超速保护使用；串口 rpm 通道则返回外环使用的 control.speed。编码器角度、累计位置与保护有效性先更新，电流零偏/ADC 检查失败不能让正常编码器反馈停止。编码器本身的无效帧仍触发保护，异常角度不进入累计与控制。</p>
</section>

<section id="pi"><div class="section-no">04 / 二自由度 PI 与抗饱和</div><h2>参考响应、扰动反馈、限幅恢复分别处理。</h2>
<p>β=0.8 只减小目标变化时比例通路的直接跳变；反馈通路的 Kp 仍为 0.1。它主要处理阶跃、超调和反转，不能单独降低匀速时的反馈噪声增益。位置模式沿用原 P 环，也经同一个速度外环输出电流目标。</p>
<div class="equation">e[k] = r[k] − v̂[k]；p[k] = βr[k] − v̂[k]<br>u<sub>PI</sub>[k] = z[k−1] + K<sub>p</sub>(p[k] − p[k−1]) + K<sub>i</sub>e[k]Δt<br>u<sub>total</sub> = u<sub>PI</sub> + u<sub>ff</sub>；Iq* = clip(u<sub>total</sub>, ±I<sub>max</sub>)<br>z[k] = u<sub>PI</sub> + min(1, K<sub>i</sub>Δt/K<sub>p</sub>)(Iq* − u<sub>total</sub>)</div>
<p>这个增量状态保存的是 PI 输出状态，不再把它误认为一个必须单独限制到 ±8 A 的“积分电流”。否则 β&lt;1 时，参考的比例权重会形成较大的等效直流状态偏置，被独立积分限幅裁掉，产生错误的稳态行为。最后一次限幅作用于 PI 加前馈的完整目标，反算也追踪这个完整目标。当前 Kp/Ki 对应约 50 ms 的反算时间常数。</p>
<p>电压侧 d/q 电流 PI 的抗饱和保持原有实现。源码目前没有加入新的调制饱和方向积分抑制；这需要在实际饱和与恢复数据中证明必要性后再做。反转制动所需的负 Iq 被保留，目标不是删除所有负电流，而是避免稳态时噪声驱动的反复换向。<a href="https://article.nadiapub.com/IJCA/vol9_no12/6.pdf" target="_blank" rel="noreferrer">Wang 等</a>的二自由度与反算研究提供方法依据，具体增益仍来自本机源码与实验。</p>
<h3>前馈补什么，何时退出？</h3>
<p>表中保存的是在相同原始绝对角度下重复出现的电流周期量，不能全部武断地命名为纯齿槽转矩。正反向低速运行可以区分部分方向相关摩擦；前三圈用于标定，第四圈独立验证，去除直流偏置后只保留跨圈与方向重复的角度成分。</p>
<div class="equation">x = θ<sub>raw</sub> × 512/360；i = floor(x) mod 512；f = x − floor(x)<br>I<sub>cog</sub>(θ<sub>raw</sub>) = table[i] + f(table[(i+1) mod 512] − table[i])<br>γ(|v̂|) = clip((200 − |v̂|)/(200 − 50), 0, 1)<br>u<sub>ff</sub> = γ × I<sub>cog</sub>(θ<sub>raw</sub>)</div>
<p>512 个 float 占 2048 字节，编译成只读表；没有新增在线学习、Flash 参数格式或串口常驻变量。zero、clear 和重启不改变 θraw 的物理索引。50～200 rpm 的线性退出是当前工程常数，<strong>尚未用补偿开关对照确定最佳区间</strong>，不能称为已经验证的边界。</p>
{figure(13)}
<p>全部第四圈原始点按插值计算的去直流残差 RMS 为正向 {M['cog_validation_1']:.5f} A、反向 {M['cog_validation_-1']:.5f} A。角度分箱均值残差看起来更小，因为它进行了平均；这两种统计不能混用。表的重复性有证据，但新闭环低速不平稳的实测说明，表能解释周期量不等于补偿组合已经成功。</p>
<p>按角度标定与回放的基本工程方法可核对 <a href="https://github.com/odriverobotics/ODrive/blob/3a2e4dd1bfbda9e4c534d5c7d8428a99c361e783/Firmware/MotorControl/controller.cpp" target="_blank" rel="noreferrer">ODrive 固定版本控制器代码</a>与 <a href="https://journals.sagepub.com/doi/abs/10.1177/0278364915599045" target="_blank" rel="noreferrer">Piccoli / Yim 的论文</a>。<a href="https://docs.odriverobotics.com/v/latest/guides/anticogging.html" target="_blank" rel="noreferrer">ODrive 官方说明</a>也提醒抗齿槽的实验性和稳定性限制，开源中存在这个功能不等于本机已经通过验收。</p>
</section>

<section id="current"><div class="section-no">05 / 电流内环实测</div><h2>d/q 电流环算法未改，补齐了真实闭环曲线。</h2>
<p>旧新两套外环均运行在 2 A 测试边界内，电流小信号实验在转矩模式进行。d 与 q 轴分别施加 0.2 A 小阶跃与 ±0.2 A PRBS，使用 20 kHz 原生 SRAM 记录，而不是拿 500 Hz 串口曲线推测几百 Hz 的电流带宽。q 轴名义工作点为 0.01 A。</p>
{figure(2)}
{figure(3)}
<p>此处幅值没有强制归一化。实际小幅电流工作点的响应与理想 600 Hz 一阶 RL 模型存在差异；16 次阶跃平均也不等于消除了所有采样误差。现有数据不足以把差异唯一归因于 R/L、PWM 死区、模拟采样误差或特定硬件故障，应继续做针对性辨识，不从曲线形状猜原因。</p>
<div class="note"><strong>采集有效性：</strong>首批临时 q 激励代码存在累积注入错误，已排除对应 q_step / q_prbs；修复临时激励并重新采集后，原生 q 目标正确为 0.01/0.21 A 或 0.01±0.2 A。页面仅使用修复后的 now_fixed 数据。此问题不影响首批正常速度/位置模式数据。临时激励已经从正式代码移除。</div>
<p>电流环原有 Clarke/Park、d/q PI、交叉耦合与反电势项、电压限制、逆变换和 PWM 更新继续保留。母线约 23.9 V。所有通过采集的工况都保留 ADC、编码器、PWM 和 UART 计数检查；小电流有效性不能作为相电流绝对测量精度的证明。</p>
</section>

<section id="dynamics"><div class="section-no">06 / 外环闭环实测</div><h2>同一个角度处理口径，比较启动、反转与位置响应。</h2>
<p>下面两套响应以 MCU 时间戳对齐。顶部曲线由原始角度统一计算速度，不用各自观测器输出作为“真实速度”；中间单独显示控制器自己的反馈；底部始终为完整 Iq 目标。角度导数使用 1 kHz、21 点三阶 Savitzky–Golay 法，两边完全相同；它仍带有角度传感器的周期误差，不是独立机械测速仪。</p>
{figure(4)}{figure(5)}
{table(['工况','10%→90% 时间，前→后','变化','50% 进度时间，前→后','同口径最大超调，前→后'],transient)}
<p>本轮启动 10%～90% 时间约 89→92 ms；反转约 168→171 ms。反转进度 50% 对应跨零，约 93→94 ms。以这次实验比较，增长都小于 10%，超调也有所减少。但这不是全部瞬态验收：没有完成多次重复、每类信号和最差段的调节时间比较，不能把“上升时间通过”写成“反转噪声问题已解决”。前一轮反转超调对照还曾出现相反结果，进一步说明重复验收必要。</p>
{figure(6)}
<p>位置外环 P=4 rpm/° 不变，速度限制 100 rpm，电流限制 2 A。1° 的小阶跃属于容易受到摩擦、量化和前馈相位影响的工况。曲线直接展示累计角度行程；不把观测器积分出的位移当作位置证据。当前结构的低速运动问题也可能在这个工况显现，不能仅依据名义位置 Bode 评价其实际效果。</p>
{figure(7)}{figure(8)}
<p>这些是<strong>参考→实际编码器输出的闭环频率响应</strong>：速度在 300 rpm 工作点施加 ±5 rpm 小扰动，位置施加 ±0.25° 扰动。速度频谱由角度输出乘 jω/6 得到，没有再套一层离线测速低通。相干性低的频点不画幅相；记录只有 4.096 秒，无法可靠刻画极低频扰动或强非线性工况。</p>
<div class="note"><strong>闭环 Bode 不等于开环稳定裕量测量。</strong>不能把这里的参考跟随幅相直接读成 50° 相位裕量或 6 dB 增益裕量。后面的开环图是明确假设下的名义模型；实物裕量仍需要更完整的本机辨识与环路注入验证。</div>
<p>本机小扰动与频率响应设计思路参考 <a href="https://arxiv.org/html/2202.08648v1" target="_blank" rel="noreferrer">Ramos Garces 等的 PMSM PI 整定研究</a>。采用的是辨识与校核方法，未复制论文中的特定 PI 数值、机械共振或陷波频率。</p>
</section>

<section id="steady"><div class="section-no">07 / 恒速结果</div><h2>高转速：目标电流波动大幅下降，平均转速保持。</h2>
{figure(9)}{figure(10)}
{table(['目标 rpm','旧 σ(Iq*) / A','新 σ(Iq*) / A','新原生短窗 σ / A','新角度平均 rpm','新 Iq / Id RMS，A','目标转矩反向占比，旧→新'],high)}
<p>高速各点 13 秒，剔除进入阶段前 2 秒，余下约 11 秒整段统计。原生短窗来自 20 kHz、4096 点（0.2048 秒），用于核查采样口径，不与 11 秒长窗假装是同一个时间范围。低于 0.02 A 的结果是完整目标自身的标准差；高速时前馈已退出，实际 Iq/Id RMS 仍明显高于目标的标准差，不能把目标变稳等同于实际电流已无噪声。</p>
<p>“目标转矩反向占比”统计 Iq* 与当前运动方向相反的样本比例；本轮高速恒速中由旧结构约 20%～50% 降至 0%。这描述稳态目标的符号行为，不把所有反向转矩都定性为错误；加减速和反转中的负 Iq 是必要的制动力。</p>
{figure(11)}
<p>同轮仅改变修正极点 200→80→40→25 rad/s，+1000 rpm 的 500 Hz 目标标准差为 0.1864→0.0491→0.0180→0.0136 A。20 kHz 原生短窗给出接近的变化趋势。该对照支持“角度修正通路是高速波动的重要来源”，但仍不能独立分离编码器非线性、真实周期负载和闭环振荡。</p>
<h3>低速：必须看角度行程，不能只看观测器输出。</h3>
{figure(12)}
{table(['目标 rpm','角度平均 rpm，旧→新','同口径角度测速 σ / rpm','完整 Iq* σ / A，旧→新','新反馈平均 rpm'],low)}
<p>±1 rpm 新旧均采集至少一完整机械圈，本轮按原始角度净行程达到 400°结束，最少 65 秒；分析排除前 2 秒后仍保留完整圈覆盖。±5、±50 rpm 使用原有短测记录，记录长度不同，未按四圈标定与重复验收要求重新采集。角度测速统一采用 500 Hz、25 点三阶 SG 导数，目标电流没有对应滤波。</p>
<div class="note bad"><strong>低速失败是实测结论。</strong>例如 +1 rpm，当前原始角度平均约 0.9573 rpm，但控制器反馈平均约 0.9985 rpm；同口径角度测速标准差约 5.73 rpm，原结构约 0.22 rpm。目标电流标准差下降不足以补偿轴的停顿与快速位移，不能按用户要求判为成功。</div>
<p>本轮尚未以新旧观测器、前馈开关和实际电流测量的独立对照定位低速退化的唯一来源。现有模型说明降低修正增益可能延迟未知扰动恢复，但不能据此直接判定是纯齿槽、摩擦、硬件或某个单一参数。下一步应优先做低速前馈开关与负载估计恢复对照；任何新参数都必须同时检查这张低速表与高速完整电流指标。</p>
</section>

<section id="frequency"><div class="section-no">08 / 频域与模型</div><h2>理论解释放在明示假设之下，不替代实测。</h2>
{figure(14)}
<p>原生角度记录中约 36 机械阶次的周期项随速度移动：1000/3000/5000 rpm 对应约 600/1800/3000 Hz。500 Hz 日志可能将其折叠至 100/200/0 Hz。即使看到 200 Hz 峰，也不能直接给外环加 200 Hz 陷波；需要先判断真实频率与扰动来源。<a href="https://docs.px4.io/main/en/config_mc/filter_tuning" target="_blank" rel="noreferrer">PX4 官方滤波说明</a>强调采样率、噪声频带与延迟的关系；其参数不能直接复制到本电机。</p>
<h3>电流环名义 Bode：前后相同。</h3>
<div class="equation">G<sub>i</sub>(s) = 1/(Ls+R)；C<sub>i</sub>(s) = K<sub>pi</sub> + K<sub>ii</sub>/s<br>L<sub>i</sub>(s) = C<sub>i</sub>G<sub>i</sub>；T<sub>i</sub>(s) = L<sub>i</sub>/(1+L<sub>i</sub>)<br>R=0.12 Ω；L=50 μH；K<sub>pi</sub>=0.1884956；K<sub>ii</sub>=452.3893</div>
{figure(15)}
<p>该简化模型忽略 PWM/ADC 数字延迟、死区、交叉耦合残差与测量噪声，名义极点抵消得到约 600 Hz。这里“600 Hz”是设计值，不是从图 3 测出的带宽；真实 FRF 的幅值与相位已经显示简化模型不完全匹配。</p>
<h3>三状态观测器不能被画成一个串联低通。</h3>
<div class="equation">P(s)=A/s；C(s)=0.1+2/s；C<sub>r</sub>(s)=0.08+2/s<br>G<sub>I→v̂</sub>(s) = As(s+3w)/(s+w)³<br>G<sub>θ→v̂</sub>(s) = [(w²/2)s²+(w³/6)s]/(s+w)³<br>H<sub>θ</sub>(s) = 6G<sub>θ→v̂</sub>(s)/s<br>G<sub>I→v̂</sub>(s) + H<sub>θ</sub>(s)P(s) = P(s)（仅机械模型完全匹配）</div>
<p>两条输入路径共同构成反馈：电流输入预测运动，角度输入修正误差。在理想匹配条件下相加得到真实机械速度通路，不能只截取角度修正项当作“4 Hz 测速低通”，否则得到的速度闭环 Bode 会是错误结构。实际惯量、摩擦、Iq 测量误差和负载不完全匹配时，这个等式不再代表真实运动反馈。</p>
{figure(16)}{figure(17)}
{table(['名义模型对象','0 dB 交越 / Hz','相位裕量 / °','增益裕量 / dB','用途'],[
 ['旧高增益结构',f"{M['nominal_margins']['before']['crossover_Hz']:.2f}",f"{M['nominal_margins']['before']['phase_margin_deg']:.2f}",f"{M['nominal_margins']['before']['gain_margin_dB']:.2f}",'说明差分测速平滑的延迟代价'],
 ['当前结构 · 理想机械模型匹配',f"{M['nominal_margins']['now']['crossover_Hz']:.2f}",f"{M['nominal_margins']['now']['phase_margin_deg']:.2f}",f"{M['nominal_margins']['now']['gain_margin_dB']:.2f}",'理论校核；不是实物裕量验收'],
 ])}
<p>速度图采用 A=4685.83 rpm/s/A、名义 600 Hz 一阶电流通路，以及 0.5 ms 的外环等效延迟假设。位置图采用 P=4 rpm/°、冻结前馈、忽略电流和速度限幅。该 0.5 ms 并非示波器量出的总延迟。实际控制中还有采样孔径、PWM 提交、编码器时序与电流测量路径；本轮没有完整测得这些延迟，因此不能以名义 67°/14 dB 宣称通过 50°/6 dB 的实物门槛。</p>
{figure(18)}{figure(19)}
<p>噪声图输入是角度误差（°），输出是 PI 电流目标（A），单位为 dB(A/°)，不是任意编码器噪声下最终电流标准差的预测。时域模型在 20 kHz 更新机械/电流/观测器、1 kHz 更新外环，保留实际离散 PI 算法；右列的未知负载阶跃展示降低修正增益的恢复代价。模型没有模拟低速静摩擦、齿槽、PWM 死区或 ADC 噪声，故不能用于宣布低速已经改善。</p>
</section>

<section id="methods"><div class="section-no">09 / 采集、保护与复现</div><h2>保留原始来源，也保留未通过的证据。</h2>
{table(['数据层','采样 / 字段','用途与限制'],[
 ['正式串口日志','20×float32 + 帧尾；84 B；500 Hz；1 Mbaud','完整 Iq*、Iq/Id、母线、角度、反馈、状态；带宽约 42%；USB 到达时间只作长段统计'],
 ['原生电流路径','20 kHz；4096×28 B；MCU μs 时间戳 + 6 float','电流参考/反馈、角度、调制比例；每窗 0.2048 s；临时 SRAM 采集'],
 ['原生外环路径','1 kHz；4096 点；MCU μs 时间戳 + 6 float','角度、完整 Iq*、实际 Iq、反馈速度、实际参考、调制比例；每窗 4.096 s'],
 ['频响估计','Welch / CSD；1024 点；50% 重叠；线性去趋势','H=Suy/Suu；相干性 γ²=|Suy|²/(Suu·Syy)；仅绘制 γ²≥0.6 的幅相'],
 ['稳态统计','进入后 2 秒以后的完整故障前区间；总体标准差 ddof=0','不挑平静片段；不额外过滤 Iq*；实际 Iq/Id RMS 同时保留'],
 ['低速运动','原始绝对角度展开；同一 SG 导数','两版同一种离线处理；无法排除传感器自身周期误差'],
 ])}
<p>正式协议没有增加诊断通道。临时 SRAM 缓冲约 112 KiB，复用来记录电流或外环数据；停机后通过 ST-Link 读出，不在运行中暂停 CPU。保护判定、ADC/PWM 提交与原控制周期保持；临时激励、采集缓冲与实验变量均已移除。Debug 与 Release 重新构建通过，最终 ELF 没有 bench_* 符号。</p>
<h3>供电告警如何处理？</h3>
<p>前轮曾在大反转或 +5000 rpm 启动时读到 CRC 正确的编码器状态位 4（欠压），已停机保留现场。本轮开始时也读到既有告警；在用户回复“OK了”后进行了受限的新一轮采集，没有把这句回复解释为已测得芯片 VDD 或已确认具体接线改动。有效新采集的编码器/ADC/PWM 时序与 UART 错误计数均为零；告警未在这些工况重现，不等于供电瞬态根因已经确认。状态与 CRC 定义可核对 <a href="https://www.magntek.com.cn/upload/pdf/202407/MT6835_Rev.1.3.pdf" target="_blank" rel="noreferrer">MT6835 Rev.1.3 数据手册</a>。</p>
<p>最终停机后回烧当前正式固件，程序区回读与文件逐字节一致，Flash 参数与校准区回读与本轮备份逐字节一致。实际带电实验采用 2 A 诊断版本；恢复的 8 A 正式版本只做停机验证，因此不能把本轮结果当成 8 A 全工况验收。</p>
{table(['最终状态','实物读回'],[
 ['板卡 / 探针','STM32F405，ID 0x413，1 MiB；ST-Link SN 8600A1002031363534313541'],
 ['串口身份','COM14；设备序列号 CB832D7DC8A1944B8A2CA4E5A51DD838'],
 ['正式固件 SHA-256',f'<code class="hash">{final["firmware_sha256"]}</code>'],
 ['正式固件长度',f'{final["firmware_bytes"]} B；程序区回读一致'],
 ['参数 / 校准区 SHA-256',f'<code class="hash">{final["parameters_sha256"]}</code>；256 KiB 备份与回读一致'],
 ['停机确认','stop 命令确认；state=0、fault=0、power_on=0、Iq*=0；采集与 OpenOCD 已结束'],
 ['最后零偏',f'B/C 均值 {final["final_telemetry"]["b_offset_V"]:.6f}/{final["final_telemetry"]["c_offset_V"]:.6f} V；原始标准差 {final["final_telemetry"]["b_std_mV"]:.3f}/{final["final_telemetry"]["c_std_mV"]:.3f} mV'],
 ])}
<div class="note bad"><strong>通过范围：</strong>本轮六个高速点单次目标波动和平均转速有改善证据，启动/反转上升时间这一次未超过 10% 余量，基础时序未增加错误。<strong>失败：</strong>低速运动连续性与周期波动。<strong>未完成：</strong>三次高速重复、全类信号最差段、调节时间、前馈退出区间、正式 300 秒、正式版本带电验收。不能将这些状态合并成一个“全部通过”。</div>
</section>

<section id="archive"><div class="section-no">10 / 数据与下载</div><h2>每张图都有矢量原稿与计算来源。</h2>
<p>图表使用 Matplotlib {M['software']['matplotlib']}、SciPy {M['software']['scipy']}、NumPy {M['software']['numpy']} 渲染。图内文字保持矢量可编辑，PNG 为 210 dpi；CSV 提供绘图使用的数据与单位。页面无需联网，图像已嵌入；PDF、SVG、PNG 和数据下载随打包文件一并保留。</p>
<div class="download-row"><a class="primary" href="FOC_algorithm_comparison.zip" download>下载网页与全部图表</a><a href="data/metrics.json" download>指标 JSON</a><a href="data/provenance.json" download>原始来源与 SHA-256</a><a href="render.py" download>绘图代码</a><a href="build_page.py" download>网页生成代码</a></div>
<details><summary>展开绘图数据 CSV 列表</summary><ul class="files">{''.join(f'<li><a href="data/{p.name}" download>{p.name}</a></li>' for p in sorted((P/'data').glob('*.csv')))}</ul></details>
<details><summary>展开原始采样来源与完整哈希</summary>{table(['工程内相对路径','字节数','SHA-256'],[[html.escape(k),v['bytes'],f'<code class="hash">{v["sha256"]}</code>'] for k,v in S.items()])}</details>
<p>原始 UART 字节、全部 CSV、采集命令与固件备份位于工程 <code>build/bench_debug/20261003_044515/</code>；前轮标定与高速基线位于 <code>20261003_030219/</code>，修正增益扫参位于 <code>20261003_040347/</code>。这三个目录不因生成网页而覆盖。网页包包含图用数据和来源哈希，原始大日志继续保留在工程中。</p>
<p>历史 <code>ad7a2e9空载.csv</code> 是 24 通道，当前日志是 20 通道；不能用当前索引解读历史字段。历史“0.015 A / 0.457 A”的用户比较来自不同轮次，未替代本页同口径的前后指标。本轮展示的所有统计都能由对应 CSV、原生二进制及绘图脚本复算。</p>
<p class="closing">本页的结论是可核对的阶段性结果：高速目标波动达到了单轮指标，低速运动未达到要求。下一步优化必须同时守住这两项，不能只让串口里的速度曲线更平。</p>
</section>
'''

style='''
:root{--ink:#243b49;--muted:#64737b;--paper:#f6f3ec;--line:#d8d9d0;--old:#ba643b;--new:#176481}*{box-sizing:border-box}html{scroll-behavior:smooth;scroll-padding-top:32px}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.85 "Microsoft YaHei","PingFang SC",sans-serif}a{color:var(--new);text-underline-offset:4px}a:hover{color:var(--old)}button{font:inherit;cursor:pointer}h1,h2,h3{font-family:"Noto Serif SC","SimSun",serif;line-height:1.35;font-weight:600}h1{font-size:clamp(34px,4.5vw,62px);letter-spacing:-1px;margin:22px 0 28px;max-width:950px}h2{font-size:clamp(25px,2.5vw,36px);margin:12px 0 30px}h3{font-size:24px;margin:36px 0 14px}p{margin:16px 0}header{border-top:8px solid var(--ink);padding:64px max(28px,calc((100vw - 1370px)/2));border-bottom:1px solid var(--line);background:#eeeae0}.eyebrow{font-size:12px;letter-spacing:3px;color:var(--muted)}.subtitle{font-size:18px;max-width:900px}.header-meta{font-size:13px;color:var(--muted);display:flex;gap:24px;flex-wrap:wrap}.actions{display:flex;gap:14px;align-items:center;margin-top:28px}.actions button,.primary{background:var(--ink);color:#fff;padding:10px 20px;border:0;text-decoration:none;font-size:14px}.actions a{font-size:14px}.layout{display:grid;grid-template-columns:220px minmax(0,1fr);gap:52px;max-width:1370px;margin:auto;padding:48px 30px 100px}aside{position:sticky;top:28px;height:fit-content;font-size:13px}aside .toc-title{border-bottom:2px solid var(--ink);padding-bottom:12px;margin-bottom:14px;letter-spacing:2px}aside a{display:block;padding:7px 0;text-decoration:none;color:var(--muted)}aside a.active{color:var(--ink);font-weight:700}aside .key{margin-top:30px;border-top:1px solid var(--line);padding-top:14px;font-size:12px}main{min-width:0}section{padding:18px 0 50px;border-bottom:1px solid var(--line);margin-bottom:40px}.section-no{font-size:12px;letter-spacing:2px;color:var(--old);font-weight:700}.lead{font-size:19px}.findings{display:grid;grid-template-columns:1fr 1fr;gap:35px;margin:36px 0}.findings>div{border-top:2px solid var(--ink);padding-top:18px}.findings strong{font-family:Georgia,serif;font-size:37px;line-height:1.4;display:block;margin-top:8px;letter-spacing:-1px}.findings small{font-size:18px;letter-spacing:0}.label{font-size:12px;color:var(--muted);letter-spacing:1px}.findings p{font-size:14px}.note{background:#eaf0ee;border-left:3px solid var(--new);padding:20px 25px;margin:26px 0;font-size:15px}.note.bad{background:#f1e7df;border-color:var(--old)}.legend{display:flex;flex-wrap:wrap;gap:18px;font-size:12px;color:var(--muted);margin:25px 0}.legend i{display:inline-block;width:22px;height:3px;margin-right:7px;vertical-align:middle}.old{background:var(--old)}.new{background:var(--new)}.equation{font-family:"Cambria Math","Microsoft YaHei",serif;background:#fffdf8;padding:24px 28px;border:1px solid var(--line);overflow:auto;line-height:2.1;font-size:17px;margin:24px 0}figure{margin:36px 0 44px;background:#fff;border:1px solid var(--line);padding:20px 24px}.figure-head{display:flex;justify-content:space-between;gap:20px;border-bottom:1px solid #e8e8e2;padding-bottom:12px;font-size:11px;letter-spacing:1.6px}.kind{color:var(--new);letter-spacing:.6px}.plot{display:block;width:100%;border:0;background:#fff;padding:18px 0 5px}.plot svg{display:block;width:100%;height:auto}figcaption{font-size:13px;border-top:1px solid #e8e8e2;padding-top:18px;color:var(--muted)}figcaption strong{font-size:16px;color:var(--ink)}figcaption p{margin:10px 0}.exports{display:flex;gap:17px;font-size:12px;margin-top:14px}.table-scroll{overflow-x:auto;margin:28px 0}table{border-collapse:collapse;min-width:100%;font-size:13px;line-height:1.65}th{text-align:left;white-space:nowrap;border-top:2px solid var(--ink);border-bottom:1px solid var(--ink);padding:12px 14px;background:#eeeae0}td{padding:12px 14px;border-bottom:1px solid var(--line);vertical-align:top}tbody tr:nth-child(even){background:#f0eee7}code{font-family:Consolas,monospace;font-size:.87em}.hash{word-break:break-all;display:inline-block;min-width:210px;max-width:650px}.download-row{display:flex;gap:15px;align-items:center;flex-wrap:wrap;margin:28px 0;font-size:13px}details{border-top:1px solid var(--line);padding:16px 0;font-size:13px}summary{cursor:pointer}.files{columns:2;padding-left:20px}.files li{break-inside:avoid;overflow-wrap:anywhere}.closing{font-family:SimSun,serif;font-size:23px;margin-top:35px}footer{background:var(--ink);color:#e2e6e4;padding:35px;text-align:center;font-size:12px;letter-spacing:1px}dialog{border:0;padding:24px;width:min(95vw,1500px);max-height:94vh;background:#fff;overflow:auto}dialog::backdrop{background:#243b49cc}dialog .zoom-top{display:flex;justify-content:space-between;gap:20px;align-items:center;margin-bottom:20px}dialog button{border:1px solid var(--line);background:#fff;padding:4px 16px}dialog svg{width:100%;height:auto}.focus:focus-visible,button:focus-visible,a:focus-visible,summary:focus-visible{outline:3px solid var(--old);outline-offset:4px}.skip{position:absolute;left:15px;top:-80px;background:#fff;padding:10px}.skip:focus{top:15px} @media(max-width:1050px){.layout{grid-template-columns:1fr;gap:15px}aside{position:static}aside nav{display:flex;flex-wrap:wrap;gap:10px 22px}aside .key{display:none}header{padding:40px 30px}.findings strong{font-size:32px}}@media(max-width:600px){body{font-size:15px}.layout{padding:26px 18px}.findings{grid-template-columns:1fr;gap:15px}figure{padding:12px;margin:25px -8px}header{padding:32px 22px}.figure-head{letter-spacing:.5px}.equation{font-size:14px;padding:15px}.files{columns:1}}@media(prefers-reduced-motion:reduce){html{scroll-behavior:auto}}@media print{@page{size:A4;margin:16mm 12mm}body{background:#fff;font-size:10pt;color:#111}header{background:#fff;padding:0 0 18px;border-top:4px solid #243b49}h1{font-size:26pt}h2{font-size:19pt}h3{font-size:14pt}.layout{display:block;padding:15px 0;margin:0}.actions,aside,.exports,.download-row,dialog,.skip{display:none}section{padding:8px 0 18px;margin-bottom:18px;break-before:auto}figure{padding:8px;margin:20px 0;break-inside:avoid}.note,.equation{break-inside:avoid}.findings strong{font-size:25pt}a{color:inherit;text-decoration:none}.table-scroll{overflow:visible}table{font-size:8pt}thead{display:table-header-group}tr{break-inside:avoid}details{display:none}footer{padding:12px;background:#fff;color:#444}p{orphans:3;widows:3}}
'''

nav=[('reading','01 结论与通过范围'),('versions','02 版本与参数'),('flow','03 算法流程'),('pi','04 PI 与角度前馈'),('current','05 电流内环实测'),('dynamics','06 外环动态与 Bode'),('steady','07 高低速恒速结果'),('frequency','08 频域解释与模型'),('methods','09 方法与最终状态'),('archive','10 数据与下载')]
page=f'''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="description" content="F405 有编码器 FOC：原高增益 PI 与三状态观测器、二自由度 PI、角度前馈的代码及实物对照。含 19 幅实测和模型矢量图，明确低速验收失败。"><title>有编码器 FOC · 算法前后对照与实测报告</title><style>{style}</style></head><body><a class="skip" href="#reading">跳到正文</a><header><div class="eyebrow">F405 / ENCODER FOC / ENGINEERING NOTE 03</div><h1>电流目标变安静了，<br>轴是否也更平稳？</h1><p class="subtitle">原高增益速度环与当前算法的代码、时域、频域和实机对照。把实际改善、低速退化和模型假设放在同一份证据中。</p><div class="header-meta"><span>2026.10.03 · 圆柱负载 / 电池供电台架</span><span>20 kHz 电流环 · 1 kHz 外环 · 500 Hz 串口</span><span>19 幅矢量图 · 离线网页</span></div><div class="actions"><button onclick="window.print()">打印 / 另存为 PDF</button><a href="#flow">先看前后算法</a><a href="#steady">直接看实测结果</a></div></header><div class="layout"><aside><div class="toc-title">CONTENTS / 目录</div><nav>{''.join(f'<a href="#{id}">{label}</a>' for id,label in nav)}</nav><div class="key">图例贯穿全文<br><span style="color:var(--old)">— 修改前高增益结构</span><br><span style="color:var(--new)">— 当前结构</span><p>点击任意图放大。<br>各图提供 SVG / PDF / PNG。</p></div></aside><main>{content}</main></div><footer>F405 FOC · SOURCE-VERIFIED / MEASURED / MODELLED · 结果按证据分开标注</footer><dialog id="zoom"><div class="zoom-top"><span id="zoom-title"></span><button id="close-zoom">关闭 ×</button></div><div id="zoom-content"></div></dialog><script>
const zoom=document.getElementById('zoom');document.querySelectorAll('.plot').forEach(b=>b.addEventListener('click',()=>{{document.getElementById('zoom-title').textContent=b.closest('figure').querySelector('figcaption strong').textContent;document.getElementById('zoom-content').innerHTML=b.innerHTML;zoom.showModal();}}));document.getElementById('close-zoom').onclick=()=>zoom.close();zoom.addEventListener('click',e=>{{if(e.target===zoom)zoom.close();}});const links=document.querySelectorAll('aside nav a');const observer=new IntersectionObserver(es=>{{es.forEach(e=>{{if(e.isIntersecting){{links.forEach(a=>a.classList.toggle('active',a.hash==='#'+e.target.id));}}}});}},{{rootMargin:'-10% 0px -65% 0px'}});document.querySelectorAll('main section').forEach(s=>observer.observe(s));
</script></body></html>'''
(P/'index.html').write_text(page,encoding='utf-8')
with zipfile.ZipFile(P/'FOC_algorithm_comparison.zip','w',zipfile.ZIP_DEFLATED) as z:
 for path in [P/'index.html',P/'render.py',P/'build_page.py',P/'manifest.json',*(P/'figures').glob('*'),*(P/'data').glob('*')]:
  z.write(path,path.relative_to(P))
print('Built offline webpage and downloadable archive:',len(page),'characters,',len(F),'figures.')
