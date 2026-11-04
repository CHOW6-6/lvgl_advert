#ifndef WEATHER_IOT_MODEL_H
#define WEATHER_IOT_MODEL_H

/* 上报到华为云 IoTDA 的服务与属性定义。
 * 这些名称需与你在华为云“产品 -> 模型定义”里定义的 serviceId / 属性名一致，
 * 否则平台会拒绝或忽略。可在产品模型里新建一个名为 weather 的服务，包含下列属性。 */
#define WEATHER_SERVICE_ID    "weather"
#define PROP_TEMPERATURE     "temperature"   /* 当前温度 ℃ */
#define PROP_HUMIDITY        "humidity"      /* 相对湿度 % */
#define PROP_WIND_SPEED      "windSpeed"     /* 风速 km/h */
#define PROP_WEATHER_TEXT    "weatherText"   /* 天气现象文本，如 "多云" */
#define PROP_CITY            "city"          /* 城市 */
#define PROP_UPDATE_TIME     "updateTime"     /* 数据更新时间 */

#endif /* WEATHER_IOT_MODEL_H */
