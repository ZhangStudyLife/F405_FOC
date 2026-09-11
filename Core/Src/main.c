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
volatile uint32_t g_mt6835_read_cycles;   /* 单次 mt6835_read() 占用的 CPU 周期 */
volatile uint32_t g_mt6835_read_ns;       /* 同上，换算成纳秒 */
volatile uint32_t g_mt6835_loops;         /* 轮询次数，用来确认循环真的在跑 */
volatile uint32_t g_mt6835_status;        /* 传感器状态位（超速/弱磁/欠压） */
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
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
#if MT6835_BRINGUP
    /* 约 1 kHz 轮询编码器。这段只用于上板自检：
         - g_mt6835_raw     手动转动磁钢时应平滑变化，范围 [0, 2097152)
         - g_mt6835_err     应恒为 0（CRC 失败 = 6，总线超时 = 5，参数错 = 2）
         - g_mt6835_read_ns 即单次 48-bit 突发读的真实耗时，用来评估 SPI 效率
       实时控制请把 MT6835_BRINGUP 置 0 关掉，改由 ADC 注入中断按 20 kHz 驱动。 */
    {
      uint32_t        t0 = bsp_time_cycles();
      mt6835_status_t st = mt6835_read(&g_mt6835);
      uint32_t        dt = bsp_time_cycles() - t0;

      g_mt6835_read_cycles = dt;
      g_mt6835_read_ns     = bsp_time_cycles_to_ns(dt);

      if (st == MT6835_OK) {
        g_mt6835_err    = 0u;
        g_mt6835_raw    = mt6835_raw(&g_mt6835);
        g_mt6835_status = mt6835_status(&g_mt6835);
      } else {
        g_mt6835_err = (uint32_t)st + 1u;
      }
      g_mt6835_loops++;
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
