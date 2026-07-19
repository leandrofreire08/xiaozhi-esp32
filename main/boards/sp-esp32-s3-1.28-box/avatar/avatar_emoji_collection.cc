#include "avatar_emoji_collection.h"
#include "lvgl_image.h"

extern const lv_image_dsc_t avatar_neutral;
extern const lv_image_dsc_t avatar_happy;
extern const lv_image_dsc_t avatar_laughing;
extern const lv_image_dsc_t avatar_funny;
extern const lv_image_dsc_t avatar_loving;
extern const lv_image_dsc_t avatar_embarrassed;
extern const lv_image_dsc_t avatar_confident;
extern const lv_image_dsc_t avatar_delicious;
extern const lv_image_dsc_t avatar_sad;
extern const lv_image_dsc_t avatar_crying;
extern const lv_image_dsc_t avatar_sleepy;
extern const lv_image_dsc_t avatar_silly;
extern const lv_image_dsc_t avatar_angry;
extern const lv_image_dsc_t avatar_surprised;
extern const lv_image_dsc_t avatar_shocked;
extern const lv_image_dsc_t avatar_thinking;
extern const lv_image_dsc_t avatar_winking;
extern const lv_image_dsc_t avatar_relaxed;
extern const lv_image_dsc_t avatar_confused;
extern const unsigned char* const avatar_blink_gif_data;
extern const unsigned int avatar_blink_gif_size;
extern const unsigned char* const avatar_speaking_gif_data;
extern const unsigned int avatar_speaking_gif_size;

AvatarEmojiCollection::AvatarEmojiCollection() {
    AddEmoji("neutral", new LvglSourceImage(&avatar_neutral));
    AddEmoji("happy", new LvglSourceImage(&avatar_happy));
    AddEmoji("laughing", new LvglSourceImage(&avatar_laughing));
    AddEmoji("funny", new LvglSourceImage(&avatar_funny));
    AddEmoji("loving", new LvglSourceImage(&avatar_loving));
    AddEmoji("embarrassed", new LvglSourceImage(&avatar_embarrassed));
    AddEmoji("confident", new LvglSourceImage(&avatar_confident));
    AddEmoji("delicious", new LvglSourceImage(&avatar_delicious));
    AddEmoji("sad", new LvglSourceImage(&avatar_sad));
    AddEmoji("crying", new LvglSourceImage(&avatar_crying));
    AddEmoji("sleepy", new LvglSourceImage(&avatar_sleepy));
    AddEmoji("silly", new LvglSourceImage(&avatar_silly));
    AddEmoji("angry", new LvglSourceImage(&avatar_angry));
    AddEmoji("surprised", new LvglSourceImage(&avatar_surprised));
    AddEmoji("shocked", new LvglSourceImage(&avatar_shocked));
    AddEmoji("thinking", new LvglSourceImage(&avatar_thinking));
    AddEmoji("winking", new LvglSourceImage(&avatar_winking));
    AddEmoji("relaxed", new LvglSourceImage(&avatar_relaxed));
    AddEmoji("confused", new LvglSourceImage(&avatar_confused));
    // Animated states (GIF). LvglRawImage(void*, size_t) reports IsGif()==true
    // when the bytes start with the "GIF" magic, and the firmware plays it via
    // LvglGif. Confirmed against 78/xiaozhi-esp32@0f6c435:
    //   lvgl_image.h:15-23 / lvgl_image.cc:12-25 (LvglRawImage + IsGif magic)
    //   lcd_display.cc:1109-1121 (SetEmotion -> IsGif() -> LvglGif).
    AddEmoji("blink", new LvglRawImage((void*)avatar_blink_gif_data, avatar_blink_gif_size));
    AddEmoji("speaking", new LvglRawImage((void*)avatar_speaking_gif_data, avatar_speaking_gif_size));
}
