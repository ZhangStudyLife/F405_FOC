/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bsp_time.h"
#include "mt6835.h"
#include "mt6835_port_stm32.h"
#include "scope.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* MT6835 上板自检（bring-up）开关。
   置 1：主循环以约 1 kHz 轮询编码器，把原始角度和单次读取耗时暴露成全局变量，
         可直接用 GDB 观察 —— 用于验证接线、校验 CRC、实测 SPI 效率。
   置 0：整段自检代码消失，主循环恢复为空，交给后续的 FOC 调度。 */
#define MT6835_BRINGUP 1

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* ---- MT6835 bring-up 观测变量（供 GDB 读取，不参与控制） ---- */
mt6835_t         g_mt6835;
volatile uint32_t g_mt6835_raw;           /* 最近一次有效采样的原始 21 bit 角度 */
volatile uint32_t g_mt6835_err;           /* 0 = 正常；否则为 mt6835_status_t + 1 */
volatile uint32_t g_mt6835_read_cycles;   /* 同步阻塞读占用的 CPU 周期 */
volatile uint32_t g_mt6835_read_ns;       /* 同步阻塞读耗时（优化前 17035 ns） */
volatile uint32_t g_mt6835_async_start_ns;  /* read_start（发起 DMA）的 CPU 开销 */
volatile uint32_t g_mt6835_async_finish_ns; /* 中间插了 6us 模拟运算后 finish 的开销 */
volatile uint32_t g_mt6835_loops;         /* 轮询次数，用来确认循环真的在跑 */
volatile uint32_t g_mt6835_status;        /* 传感器状态位（超速/弱磁/欠压） */

/* ---- RAM 示波器：目标板自己采样，主机用 tools/scope_capture.py 经 SWD 抓走 ----
   通道分配（bring-up 阶段，采样率 ≈ 1 kHz）：
     ch0 = 位置，毫度 [0, 360000)      ← 转动电机轴时应是平滑锯齿
     ch1 = 速度，rpm×100（已低通）      ← 匀速转动时应是水平线
     ch2 = 速度，rpm×100（未滤波）      ← 用它看量化噪声有多大，决定低通多重
     ch3 = 原始 21 bit 计数值
     ch4 = 芯片给的 CRC
     ch5 = 同步阻塞读耗时 ns
   注意：ch0~ch2 都放大成整数存放（scope 通道是 int32）。 */
scope_t          g_scope;
volatile int32_t g_mt6835_pos_mdeg;      /* 位置，毫度 */
volatile int32_t g_mt6835_speed_rpm100;  /* 速度，rpm×100（滤波后） */
volatile int32_t g_mt6835_speed_raw100;  /* 速度，rpm×100（未滤波） */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM8_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_SPI3_Init();
  /* USER CODE BEGIN 2 */
  /* DWT 周期计数器：必须在任何 bsp_time_* 之前初始化一次 */
  bsp_time_init();

#if MT6835_BRINGUP
  /* 绑定 SPI3 + PA0 并建链（内部含上电等待与重试，约 20~30 ms） */
  if (!mt6835_port_stm32_bind(&g_mt6835, &hspi3,
                              MT6835_CS_GPIO_Port, MT6835_CS_Pin)) {
    g_mt6835_err = 1u;   /* 建链失败：排查见 App/Hardware/mt6835/README.md */
  }
#endif

  /* RAM 示波器：采样率就是下面主循环的轮询频率（约 1 kHz）。
     真正跑 FOC 时改成 20000，并在 ADC 注入中断里调 scope_push()。 */
  scope_init(&g_scope, 1000u);

  /* 测速低通：1 kHz 采样下取 20 Hz 截止，α≈0.12，时间常数约 8 ms。
     跑 20 kHz 时换成 mt6835_set_speed_filter_hz(&g_mt6835, 200.0f, 20000.0f)。 */
  mt6835_set_speed_filter_hz(&g_mt6835, 20.0f, 1000.0f);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
#if MT6835_BRINGUP
    /* 约 1 kHz 轮询编码器，同时对比阻塞路径与 DMA 异步路径的开销。
         - g_mt6835_read_ns        同步阻塞读（寄存器级快路径）
         - g_mt6835_async_start_ns 发起 DMA 的 CPU 开销
         - g_mt6835_async_finish_ns 中间插了 6us "模拟 FOC 运算" 后收取结果的开销
       实时控制请把 MT6835_BRINGUP 置 0 关掉，改由 ADC 注入中断按 20 kHz 驱动。 */
    {
      uint32_t        t0;
      uint32_t        dt;
      mt6835_status_t st;

      /* --- 1) 同步阻塞读 --- */
      t0 = bsp_time_cycles();
      (void)mt6835_read(&g_mt6835);
      dt = bsp_time_cycles() - t0;
      g_mt6835_read_cycles = dt;
      g_mt6835_read_ns     = bsp_time_cycles_to_ns(dt);

      /* --- 2) 异步 DMA 读：start 之后先干别的活，再收结果 --- */
      t0 = bsp_time_cycles();
      st = mt6835_read_start(&g_mt6835);
      g_mt6835_async_start_ns = bsp_time_cycles_to_ns(bsp_time_cycles() - t0);

      if (st == MT6835_OK) {
        /* 这段时间 SPI 在后台搬数据，等价于 FOC 的 Clarke/Park/PI/SVPWM 运算 */
        bsp_time_delay_us(6u);

        t0 = bsp_time_cycles();
        st = mt6835_read_finish(&g_mt6835);
        g_mt6835_async_finish_ns = bsp_time_cycles_to_ns(bsp_time_cycles() - t0);
      } else {
        g_mt6835_async_finish_ns = 0u;
        g_mt6835_err = 0xFFFFFFFFu;   /* 异步发起就失败了，单独标记 */
      }

      if (st == MT6835_OK) {
        g_mt6835_err    = 0u;
        g_mt6835_raw    = mt6835_raw(&g_mt6835);
        g_mt6835_status = mt6835_status(&g_mt6835);
      } else if (g_mt6835_err != 0xFFFFFFFFu) {
        g_mt6835_err    = (uint32_t)st + 1u;
        g_mt6835_status = 0u;
      }
      g_mt6835_loops++;

      /* 推一组样本进 RAM 示波器（约 25 周期，对控制环无影响）。
         位置/速度都放大成整数：毫度、rpm×100 —— scope 通道是 int32，
         直接塞浮点会丢精度，主机侧用 --div 除回来即可。 */
      g_mt6835_pos_mdeg     = (int32_t)(mt6835_angle_deg(&g_mt6835) * 1000.0f);
      g_mt6835_speed_rpm100 = (int32_t)(mt6835_speed_rpm(&g_mt6835) * 100.0f);
      g_mt6835_speed_raw100 =
          (int32_t)(mt6835_speed_raw_rad_s(&g_mt6835) * 9.5492965855f * 100.0f);
      {
        int32_t sv[SCOPE_CHANNELS];
        sv[0] = g_mt6835_pos_mdeg;
        sv[1] = g_mt6835_speed_rpm100;
        sv[2] = g_mt6835_speed_raw100;
        sv[3] = (int32_t)g_mt6835.sample.raw;
        sv[4] = (int32_t)g_mt6835.sample.crc;
        sv[5] = (int32_t)g_mt6835_read_ns;
        scope_push(&g_scope, sv);
      }
    }
    HAL_Delay(1);
#endif
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
