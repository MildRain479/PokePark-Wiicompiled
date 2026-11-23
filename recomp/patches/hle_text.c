/* Minigame instructions for the PC's controls. Each minigame's lobby
 * explains how to play (Lobby.bmg, the same in every lobby), and most of
 * it is about the Wii Remote: holding it upright, shaking it, tilting
 * it, pointing it. Here those lines are replaced, as the game looks them
 * up, by ones that say what to press (hle_motion.c gives the remote's
 * motion for it), on the keyboard or the controller, whichever was used
 * last, or with gyro aiming or steering on (and a gyroscope), what to
 * tilt or turn, and the stick that does the same. So is Primeape's, at
 * the Meeting Place, on using Iron Tail. Each is written in the language
 * the text started in: English, or that of a language pack (the
 * Japanese and European discs' Lobby.bmg have the same messages, in the
 * same order). Buttons are shown with the game's own button tags, which
 * hle_prompts.c draws as the keys or controller buttons bound to them.
 * The movement and shake keys are named instead: the d-pad's glyph, four
 * keys in a cross, is too small to read in a sentence, and the shake keys
 * have none. The pictures beside the text still show a Wii Remote.
 *
 * The new text is kept in guest memory taken from the top of the arena
 * at boot, and written again whenever the bindings or the device change.
 * A message is a big-endian UTF-16 string; a tag is 0x1A, its length in
 * bytes and group (one byte each), then its data: group 5 is a button,
 * shown as its glyph (U+E040 + the button: 2 A, 3 B, 7 1, 8 2). */
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "host_input.h"

void (*g_host_button_name)(uint32_t button, char* out, int n);
int (*g_host_motion_text)(int* handheld);
int (*g_host_text_language)(void);

#define TEXT_SIZE 0x4000
static uint32_t sBase;

/* Called at boot, after the FST has taken the top of MEM1's arena. */
void hle_text_boot_setup(void) {
    uint32_t arena_hi = LD32(0x80000034);
    sBase = (arena_hi - TEXT_SIZE) & ~31u;
    ST32(0x80000034, sBase); /* arena hi */
    ST32(0x80003110, sBase); /* MEM1 arena end */
}

/* The words the new text is put together with, in each language: the
 * shake keys' names joined ("Q and E", "Q or E") or, with none bound,
 * named; what has the gyroscope, as the object of a verb ("the
 * controller", "the device" for the handheld itself); and an unbound
 * key. */
static const struct {
    const char *both, *either, *shakeKeys, *aShakeKey, *controller, *handheld, *nothing;
} kWords[HOST_TEXTS] = {
    {" and ", " or ", "the shake buttons", "a shake button", "the controller", "the device", "nothing"},
    {"と", "か", "ふるボタン", "ふるボタン", "コントローラー", "ほんたい", "なし"},
    {" et ", " ou ", "les boutons de secousse", "un bouton de secousse", "la manette", "l'appareil", "rien"},
    {" und ", " oder ", "die Schüttel-Tasten", "eine Schüttel-Taste", "den Controller", "das Gerät", "nichts"},
    {" e ", " o ", "i tasti per scuotere", "un tasto per scuotere", "il controller", "il dispositivo", "niente"},
    {" y ", " o ", "los botones de agitar", "un botón de agitar", "el mando", "el dispositivo", "nada"},
};

/* In the new text (UTF-8): [A] [B] [1] [2] are those buttons' glyphs; [it]
 * what has the gyroscope; [up] [down] [left] [right] the movement keys'
 * names, [shake] the two shake keys' ("Q and E"), [either shake] the same
 * with "or"; [box] the tag that the field's messages begin with (a message
 * box's, 8 bytes; it is passed over in matching) and [page] the one that
 * goes on to the box's next page. Each replaces the message at index in
 * the text of lang whose text begins with was (checked so that no other
 * file's are replaced): key on the keyboard, pad (if different) on the
 * controller, and gyro, whichever was used last, while the motion it needs
 * (HOST_MOTION_*) is on. PASSWORD marks the password screen's "incorrect",
 * replaced only when the Unlock all button says it (hle_pause.c). */
#define AIM HOST_MOTION_AIM
#define STEER HOST_MOTION_STEER
#define PASSWORD 0x100
#define EN HOST_TEXT_EN
#define JP HOST_TEXT_JP
#define FR HOST_TEXT_FR
#define DE HOST_TEXT_DE
#define IT HOST_TEXT_IT
#define ES HOST_TEXT_ES
static const struct {
    int lang, index;
    const char *was, *key, *pad;
    int motion;
    const char* gyro;
} kText[] = {
    /* English */
    {EN, 17, "Password is incorrect!", "All passwords entered!", NULL, PASSWORD},
#define EN_RUN "Press [shake] by turns\nto start running. The faster you\npress them, the faster you'll go!"
    {EN, 20, "Shake the Wii Remote", EN_RUN},
    {EN, 23, "Once you've cleared the final hurdle,",
     "Once you've cleared the final hurdle,\npress [shake] by turns as quickly\nas you can to sprint to the finish line!"},
    {EN, 26, "When you begin, tilt", "When you begin, steer with [left] and [right].\nBe careful not to fall!",
     "When you begin, steer with the left stick.\nBe careful not to fall!"},
    {EN, 31, "Once you reach Rayquaza's head,", "Once you reach Rayquaza's head,\nyou'll see a Pointer.\nMove it with the mouse.",
     "Once you reach Rayquaza's head,\nyou'll see a Pointer.\nMove it with the right stick.", AIM,
     "Once you reach Rayquaza's head,\nyou'll see a Pointer. Move it by turning\n[it] or with the right stick."},
    {EN, 35, "Hold the Wii Remote vertically", "When the time starts, press [shake]\nby turns to swing on the vine!"},
    {EN, 37, "Once you are swinging well,", "Once you are swinging well,\nit's time to jump!\nPress [B]."},
    {EN, 41, "Hold the Wii Remote vertically", "When the time starts, press [shake]\nby turns to swing on the rope!"},
    {EN, 42, "Swing in time with the rope", "Swing in time with the rope\nand jump when ready.\nPress [B] to jump."},
    {EN, 46, "Shake the Wii Remote", EN_RUN},
    {EN, 48, "You'll slow down when you",
     "You'll slow down when you\nsmash into blocks. Keep pressing\n[shake] by turns to keep your speed up!"},
#define EN_TURN_KEY "Press [left] or [right]\nwhen you want to make a turn."
#define EN_TURN_PAD "Push the left stick left or right\nwhen you want to make a turn."
#define EN_TURN_GYRO "Tilt [it] like a wheel, or push\nthe left stick left or right,\nwhen you want to make a turn."
#define EN_CLIMB_KEY "Press [up] to fly higher\nor [down] to fly lower."
#define EN_CLIMB_PAD "Push the left stick up to fly higher\nor down to fly lower."
#define EN_CLIMB_GYRO \
    "Tilt [it] back or push the left\nstick up to fly higher; tilt it forward\nor push the stick down to fly lower."
    {EN, 52, "Tilt the Wii Remote", EN_TURN_KEY, EN_TURN_PAD, STEER, EN_TURN_GYRO},
    {EN, 59, "Tilt your Wii Remote left or right", "Press [left] or [right] if you want\nto move to the left or right.",
     "Push the left stick left or right if\nyou want to move in those directions.", STEER,
     "Tilt [it] like a wheel, or push\nthe left stick, to move left or right."},
    {EN, 60, "Tilt your Wii Remote backward", EN_CLIMB_KEY, EN_CLIMB_PAD, STEER, EN_CLIMB_GYRO},
    {EN, 66, "Tilt the Wii Remote left or right", EN_TURN_KEY, EN_TURN_PAD, STEER, EN_TURN_GYRO},
    {EN, 80, "Tilt the Wii Remote as shown", "Use [up], [left], [down] and [right]\nto move around when you begin.\nAvoid falling off!",
     "Use the left stick to move around\nwhen you begin.\nAvoid falling off!", STEER,
     "Tilt [it] or use the left stick\nto roll around when you begin.\nAvoid falling off!"},
    {EN, 88, "Hold the Wii Remote vertically.", "When the game begins, boulders will\nstart to fly toward you!"},
    {EN, 89, "Bash boulders by swinging",
     "Bash boulders by swinging with [1] or [2].\nThe better your swing is timed,\nthe more points you'll score!"},
    {EN, 90, "You won't be able to move",
     "You won't be able to move for a while if a\nboulder hits you! Press [shake] by turns\nto recover as quickly as you can!"},
    {EN, 95, "Point the Wii Remote at the screen.",
     "Move the Pointer with the mouse.\nTake it past the side of the screen\nto turn that way.",
     "Move the Pointer with the right stick.\nTake it past the side of the screen\nto turn that way.", AIM,
     "Turn [it] or use the right\nstick to move the Pointer. Go past\nthe screen's side to turn that way."},
    {EN, 96, "Push ", "Press [A] to shoot!\nKeep Ghost types away and score points."},
    {EN, 103, "Tilt the Wii Remote left or right", EN_TURN_KEY, EN_TURN_PAD, STEER, EN_TURN_GYRO},
    {EN, 104, "Tilt your Wii Remote backward", EN_CLIMB_KEY, EN_CLIMB_PAD, STEER, EN_CLIMB_GYRO},
    {EN, 111, "Shake the Wii Remote", EN_RUN},
    /* Primeape, having taught Iron Tail (the Meeting Place's
     * Ar02Zn01Dat.dac, message 0x150001F4) */
    {EN, 51, "You can use Iron Tail in battle.",
     "[box]You can use Iron Tail in battle.\nPress [either shake], and you will\ndeliver a blow to the Pokémon\nin front of you!"},

    /* Japanese: the game's own kana, words spaced with ideographic spaces */
    {JP, 17, "パスワードが", "パスワードを　ぜんぶ　いれたよ！", NULL, PASSWORD},
#define JP_RUN "スタートで　はしりだしたら\n[shake]を　こうごに　おそう。\nはやく　おせば　スピードアップ！"
    {JP, 20, "スタートで", JP_RUN},
    {JP, 23, "さいごの　ハードル", "さいごの　ハードルを　とびこえたら\n[shake]を　こうごに　はやく　おして\nゴールまで　かけぬけよう！"},
    {JP, 26, "スタートで", "スタートで　はしりだしたら\n[left]と[right]で　かじをとって\nおちないように　すすもう。",
     "スタートで　はしりだしたら\nひだりスティックで　かじをとって\nおちないように　すすもう。"},
    {JP, 31, "レックウザの", "レックウザの　あたまに　ついたら\nポインターが　でるぞ。\nマウスで　うごかそう。",
     "レックウザの　あたまに　ついたら\nポインターが　でるぞ。\nみぎスティックで　うごかそう。", AIM,
     "レックウザの　あたまに　ついたら\n[it]か　みぎスティックで\nポインターを　うごかそう。"},
    {JP, 35, "Wiiリモコンを", "スタートしたら　[shake]を\nこうごに　おして　つるで　ゆれよう！"},
    {JP, 41, "Wiiリモコンを", "スタートしたら　[shake]を\nこうごに　おして　ロープで　ゆれよう！"},
    {JP, 46, "スタートで", JP_RUN},
    {JP, 48, "しょうがいぶつに", "しょうがいぶつに　ぶつかると\nスピードが　おちるぞ。　[shake]を\nこうごに　おして　スピードアップ！"},
#define JP_TURN_KEY "まがるときは　まがりたい　ほうの\n[left]か[right]を　おそう。"
#define JP_TURN_PAD "まがるときは　ひだりスティックを\nまがりたい　むきに　たおそう。"
#define JP_TURN_GYRO "まがるときは　[it]を\nハンドルのように　かたむけるか\nひだりスティックを　たおそう。"
#define JP_SIDE_KEY "よこに　うごきたいときは\n[left]か[right]を　おそう。"
#define JP_SIDE_PAD "よこに　うごきたいときは\nひだりスティックを　みぎ　ひだりに\nたおそう。"
#define JP_SIDE_GYRO "[it]を　ハンドルのように\nかたむけるか　ひだりスティックを\nよこに　たおして　うごこう。"
#define JP_CLIMB_KEY "うえや　したに　うごきたいときは\n[up]か[down]を　おそう。"
#define JP_CLIMB_PAD "うえや　したに　うごきたいときは\nひだりスティックを　うえ　したに\nたおそう。"
#define JP_CLIMB_GYRO "[it]を　まえ　うしろに\nかたむけるか　ひだりスティックを\nうえ　したに　たおして　うごこう。"
    {JP, 52, "まがるときは", JP_TURN_KEY, JP_TURN_PAD, STEER, JP_TURN_GYRO},
    {JP, 59, "よこに", JP_SIDE_KEY, JP_SIDE_PAD, STEER, JP_SIDE_GYRO},
    {JP, 60, "うえや", JP_CLIMB_KEY, JP_CLIMB_PAD, STEER, JP_CLIMB_GYRO},
    {JP, 66, "まがるときは", JP_TURN_KEY, JP_TURN_PAD, STEER, JP_TURN_GYRO},
    {JP, 80, "スタートしたら", "スタートしたら　[up]、[left]、[down]、[right]で\nおちないように　うごきまわろう。",
     "スタートしたら　ひだりスティックで\nおちないように　うごきまわろう。", STEER,
     "スタートしたら　[it]を\nかたむけるか　ひだりスティックで\nおちないように　うごきまわろう。"},
    {JP, 88, "Wiiリモコンを", "スタートしたら　いわが　とんでくるぞ。"},
    {JP, 89, "いわが　とんできたら",
     "いわが　とんできたら\n[1]か[2]ボタンで　こうげきだ！\nタイミングよく　こわせば　こうとくてん。"},
    {JP, 90, "いわに　あたると", "いわに　あたると　しばらく\nうごけなくなるぞ。　[shake]を\nこうごに　おして　かいふくだ！"},
    {JP, 95, "Wiiリモコンを", "ポインターを　マウスで\nうごかそう。　はしまで　もっていくと\nそっちを　むくぞ。",
     "ポインターを　みぎスティックで\nうごかそう。　はしまで　もっていくと\nそっちを　むくぞ。", AIM,
     "[it]か　みぎスティックで\nポインターを　うごかそう。　はしまで\nもっていくと　そっちを　むくぞ。"},
    {JP, 103, "よこに", JP_SIDE_KEY, JP_SIDE_PAD, STEER, JP_SIDE_GYRO},
    {JP, 104, "うえや", JP_CLIMB_KEY, JP_CLIMB_PAD, STEER, JP_CLIMB_GYRO},
    {JP, 111, "スタートで", JP_RUN},
    {JP, 51, "アイアンテールは",
     "[box]アイアンテールは\nバトルで　つかえるわざ。[page]\n[either shake]を　おすと\nめのまえの　あいてを\nはねとばすことが　できる。"},

    /* French */
    {FR, 17, "Le code est incorrect", "Tous les codes ont été entrés!", NULL, PASSWORD},
#define FR_RUN "Appuie tour à tour sur [shake]\ndès le signal du départ!\nPlus tu appuies vite, plus tu iras vite!"
    {FR, 20, "Secoue la télécommande Wii", FR_RUN},
    {FR, 23, "Quand tu franchis le dernier",
     "Quand tu franchis le dernier obstacle,\nappuie tour à tour sur [shake] aussi\nvite que possible jusqu'à l'arrivée!"},
    {FR, 26, "Incline la télécommande Wii", "Dès le départ, dirige-toi avec [left]\net [right]. Attention à ne pas tomber!",
     "Dès le départ, dirige-toi avec le\nstick gauche. Attention à ne pas tomber!"},
    {FR, 31, "Une fois arrivé", "Une fois sur la tête de Rayquaza,\nun pointeur apparaît.\nDéplace-le avec la souris.",
     "Une fois sur la tête de Rayquaza,\nun pointeur apparaît.\nDéplace-le avec le stick droit.", AIM,
     "Une fois sur la tête de Rayquaza, un\npointeur apparaît. Tourne [it] ou\nutilise le stick droit pour le déplacer."},
#define FR_SWING "Dès le départ, appuie tour à tour\nsur [shake] pour te balancer!"
    {FR, 35, "Secoue la télécommande Wii", FR_SWING},
    {FR, 41, "Secoue la télécommande Wii", FR_SWING},
    {FR, 46, "Secoue la télécommande Wii", FR_RUN},
    {FR, 48, "Quand tu casses un mur",
     "Quand tu casses un mur, tu ralentis.\nAppuie vite tour à tour sur [shake]\npour accélérer!"},
#define FR_TURN_KEY "Dans un virage, appuie sur [left]\nou [right] selon la direction\noù tu veux tourner."
#define FR_TURN_PAD "Dans un virage, pousse le stick gauche\nvers la gauche ou vers la droite."
#define FR_TURN_GYRO "Dans un virage, incline [it] comme\nun volant, ou pousse le stick gauche\nvers la gauche ou vers la droite."
#define FR_SIDE_KEY "Pour changer de trajectoire,\nappuie sur [left] ou [right]."
#define FR_SIDE_PAD "Pour changer de trajectoire, pousse\nle stick gauche vers la gauche ou\nvers la droite."
#define FR_SIDE_GYRO "Pour changer de trajectoire, incline\n[it] comme un volant, ou pousse\nle stick gauche sur le côté."
#define FR_CLIMB_KEY "Appuie sur [up] pour monter\net sur [down] pour descendre."
#define FR_CLIMB_PAD "Pousse le stick gauche vers le haut\npour monter et vers le bas\npour descendre."
#define FR_CLIMB_GYRO \
    "Pour monter, incline [it] vers\nl'arrière ou pousse le stick gauche vers\nle haut. Fais l'inverse pour descendre."
    {FR, 52, "Dans un virage", FR_TURN_KEY, FR_TURN_PAD, STEER, FR_TURN_GYRO},
    {FR, 59, "Pour changer de trajectoire", FR_SIDE_KEY, FR_SIDE_PAD, STEER, FR_SIDE_GYRO},
    {FR, 60, "Pour monter ou descendre", FR_CLIMB_KEY, FR_CLIMB_PAD, STEER, FR_CLIMB_GYRO},
    {FR, 66, "Pour prendre les virages", FR_TURN_KEY, FR_TURN_PAD, STEER, FR_TURN_GYRO},
    {FR, 80, "Dès que le jeu commence",
     "Dès le départ, déplace-toi avec\n[up], [left], [down] et [right].\nAttention à ne pas tomber!",
     "Dès le départ, déplace-toi avec\nle stick gauche.\nAttention à ne pas tomber!", STEER,
     "Dès le départ, incline [it] ou\nutilise le stick gauche pour rouler.\nAttention à ne pas tomber!"},
    {FR, 88, "Tiens la télécommande Wii", "Dès le début du jeu, des rochers\nvont foncer sur toi!"},
    {FR, 89, "Secoue la télécommande Wii pour briser",
     "Brise les rochers en frappant avec [1]\nou [2]! Fais-le au dernier moment\npour gagner encore plus de points!"},
    {FR, 90, "Si tu te fais toucher",
     "Si tu te fais toucher par un rocher,\ntu es paralysé un moment. Appuie tour\nà tour sur [shake] pour te rétablir!"},
    {FR, 95, "Tiens la télécommande Wii",
     "Déplace le pointeur avec la souris.\nAmène-le au bord de l'écran\npour tourner de ce côté.",
     "Déplace le pointeur avec le stick droit.\nAmène-le au bord de l'écran\npour tourner de ce côté.", AIM,
     "Tourne [it] ou utilise le stick\ndroit pour déplacer le pointeur. Va au\nbord de l'écran pour tourner."},
    {FR, 103, "Pour changer de trajectoire", FR_SIDE_KEY, FR_SIDE_PAD, STEER, FR_SIDE_GYRO},
    {FR, 104, "Pour monter ou descendre", FR_CLIMB_KEY, FR_CLIMB_PAD, STEER, FR_CLIMB_GYRO},
    {FR, 111, "Secoue la télécommande Wii", FR_RUN},
    {FR, 51, "Queue de Fer",
     "[box]Queue de Fer est une capacité\nà utiliser en duel.[page]\nAppuie sur [either shake] pour frapper\navec vigueur un adversaire devant toi."},

    /* German */
    {DE, 17, "Das Passwort ist falsch", "Alle Passwörter eingegeben!", NULL, PASSWORD},
#define DE_RUN "Drücke nach dem Startschuss abwechselnd\n[shake]! Je schneller du drückst,\ndesto schneller wirst du!"
    {DE, 20, "Schüttele die Wii-Fernbedienung", DE_RUN},
    {DE, 23, "Lass die letzte Hürde",
     "Lass die letzte Hürde hinter dir und\ndrücke so schnell du kannst abwechselnd\n[shake], um das Ziel zu erreichen!"},
    {DE, 26, "Halte nach dem Startschuss", "Steuere nach dem Start mit [left] und\n[right] und versuche, nicht zu fallen!",
     "Steuere nach dem Start mit dem linken\nStick und versuche, nicht zu fallen!"},
    {DE, 31, "Hast du den Bug", "Hast du Rayquazas Kopf erreicht,\nerscheint der Cursor. Bewege ihn\nmit der Maus!",
     "Hast du Rayquazas Kopf erreicht,\nerscheint der Cursor. Bewege ihn\nmit dem rechten Stick!", AIM,
     "Bei Rayquazas Kopf erscheint der Cursor.\nDrehe [it] oder nutze den\nrechten Stick, um ihn zu bewegen!"},
#define DE_SWING "Drücke nach dem Start abwechselnd\n[shake], um zu schwingen!"
    {DE, 35, "Halte die Wii-Fernbedienung aufrecht", DE_SWING},
    {DE, 41, "Halte die Wii-Fernbedienung aufrecht", DE_SWING},
    {DE, 46, "Schüttele die Wii-Fernbedienung", DE_RUN},
    {DE, 48, "Bleibst du an",
     "Bleibst du an einer Hürde hängen,\nwirst du langsamer. Drücke abwechselnd\n[shake] für mehr Tempo!"},
#define DE_TURN_KEY "Drücke [left] oder [right],\num Kurven zu nehmen."
#define DE_TURN_PAD "Drücke den linken Stick nach links\noder rechts, um Kurven zu nehmen."
#define DE_TURN_GYRO "Neige [it] wie ein Lenkrad\noder drücke den linken Stick nach links\noder rechts, um Kurven zu nehmen."
#define DE_SIDE_KEY "Drücke [left] oder [right], um dich\nhorizontal über das Spielfeld zu bewegen."
#define DE_SIDE_PAD "Drücke den linken Stick nach links oder\nrechts, um dich horizontal über das\nSpielfeld zu bewegen."
#define DE_SIDE_GYRO "Neige [it] wie ein Lenkrad\noder drücke den linken Stick zur Seite,\num dich horizontal zu bewegen."
#define DE_CLIMB_KEY "Drücke [up], um aufzusteigen,\nund [down], um abzusinken."
#define DE_CLIMB_PAD "Drücke den linken Stick nach oben, um\naufzusteigen, und nach unten, um\nabzusinken."
#define DE_CLIMB_GYRO \
    "Neige [it] nach hinten\noder drücke den linken Stick nach oben,\num aufzusteigen. Umgekehrt sinkst du ab."
    {DE, 52, "Neige die Wii-Fernbedienung", DE_TURN_KEY, DE_TURN_PAD, STEER, DE_TURN_GYRO},
    {DE, 59, "Neige die Wii-Fernbedienung", DE_SIDE_KEY, DE_SIDE_PAD, STEER, DE_SIDE_GYRO},
    {DE, 60, "Neige die Wii-Fernbedienung", DE_CLIMB_KEY, DE_CLIMB_PAD, STEER, DE_CLIMB_GYRO},
    {DE, 66, "Neige die Wii-Fernbedienung", DE_TURN_KEY, DE_TURN_PAD, STEER, DE_TURN_GYRO},
    {DE, 80, "Bewege die Wii-Fernbedienung",
     "Bewege dich nach Spielbeginn mit [up],\n[left], [down] und [right], ohne\nvom Spielfeld zu fallen!",
     "Bewege dich nach Spielbeginn mit dem\nlinken Stick, ohne vom Spielfeld\nzu fallen!", STEER,
     "Neige nach Spielbeginn [it] oder\nnutze den linken Stick, um zu rollen,\nohne vom Spielfeld zu fallen!"},
    {DE, 88, "Halte die Wii-Fernbedienung aufrecht", "Bei Spielbeginn heißt es „Feuer frei“!"},
    {DE, 89, "Erbeute Punkte",
     "Erbeute Punkte, indem du rechtzeitig\nmit [1] oder [2] zuschlägst, wenn\nein Fels auf dich zukommt!"},
    {DE, 90, "Ein Felstreffer",
     "Ein Felstreffer lähmt dich kurzzeitig.\nDrücke abwechselnd [shake], um\nwieder zu Sinnen zu kommen!"},
    {DE, 95, "Ziele mit der",
     "Bewege den Cursor mit der Maus! Führe\nihn über den Bildschirmrand hinaus,\num dich in diese Richtung zu drehen.",
     "Bewege den Cursor mit dem rechten Stick!\nFühre ihn über den Bildschirmrand,\num dich in diese Richtung zu drehen.", AIM,
     "Drehe [it] oder nutze den rechten\nStick, um den Cursor zu bewegen. Am\nRand drehst du dich dorthin."},
    {DE, 103, "Neige die Wii-Fernbedienung", DE_SIDE_KEY, DE_SIDE_PAD, STEER, DE_SIDE_GYRO},
    {DE, 104, "Neige die Wii-Fernbedienung", DE_CLIMB_KEY, DE_CLIMB_PAD, STEER, DE_CLIMB_GYRO},
    {DE, 111, "Schüttele die Wii-Fernbedienung", DE_RUN},
    {DE, 51, "Die Attacke Eisenschweif",
     "[box]Die Attacke Eisenschweif kommt\nnur in Kämpfen zum Einsatz.[page]\nDrücke [either shake], um deinen\nGegner zurückzustoßen."},

    /* Italian */
    {IT, 17, "Password errata", "Tutte le password inserite!", NULL, PASSWORD},
#define IT_RUN "Premi [shake] alternandoli per iniziare\na correre. Più premi veloce,\npiù vai veloce!"
    {IT, 20, "Scuoti il telecomando Wii", IT_RUN},
    {IT, 23, "Una volta saltato",
     "Una volta saltato l'ultimo ostacolo,\npremi [shake] alternandoli a più non\nposso fino al traguardo!"},
    {IT, 26, "Al via, fai oscillare", "Al via, sterza con [left] e [right].\nFai attenzione a non cadere!",
     "Al via, sterza con la levetta sinistra.\nFai attenzione a non cadere!"},
    {IT, 31, "Una volta sulla testa",
     "Una volta sulla testa di Rayquaza,\nvedrai apparire un puntatore.\nSpostalo con il mouse.",
     "Una volta sulla testa di Rayquaza,\nvedrai apparire un puntatore.\nSpostalo con la levetta destra.", AIM,
     "Una volta sulla testa di Rayquaza,\nvedrai un puntatore. Spostalo ruotando\n[it] o con la levetta destra."},
#define IT_SWING "Al via, premi [shake] alternandoli\nper oscillare!"
    {IT, 35, "Punta il telecomando Wii", IT_SWING},
    {IT, 41, "Punta il telecomando Wii", IT_SWING},
    {IT, 46, "Scuoti il telecomando Wii", IT_RUN},
    {IT, 48, "Quando colpisci",
     "Quando colpisci degli ostacoli perdi\nvelocità. Premi [shake] alternandoli\nper accelerare a più non posso!"},
#define IT_TURN_KEY "Premi [left] o [right]\nnella direzione in cui vuoi girare."
#define IT_TURN_PAD "Inclina la levetta sinistra\nnella direzione in cui vuoi girare."
#define IT_TURN_GYRO "Inclina [it] come un volante,\no la levetta sinistra, nella\ndirezione in cui vuoi girare."
#define IT_SIDE_KEY "Se vuoi muoverti verso destra o\nsinistra, premi [left] o [right]."
#define IT_SIDE_PAD "Se vuoi muoverti verso destra o\nsinistra, inclina la levetta sinistra\nin una direzione o nell'altra."
#define IT_SIDE_GYRO "Se vuoi muoverti verso destra o\nsinistra, inclina [it] come un\nvolante, o la levetta sinistra."
#define IT_CLIMB_KEY "Premi [up] per salire\ne [down] per scendere."
#define IT_CLIMB_PAD "Inclina la levetta sinistra in alto\nper salire e in basso per scendere."
#define IT_CLIMB_GYRO "Inclina [it] indietro\no la levetta sinistra in su per salire.\nFai il contrario per scendere."
    {IT, 52, "Inclina il telecomando Wii", IT_TURN_KEY, IT_TURN_PAD, STEER, IT_TURN_GYRO},
    {IT, 59, "Se vuoi muoverti", IT_SIDE_KEY, IT_SIDE_PAD, STEER, IT_SIDE_GYRO},
    {IT, 60, "Quando vuoi spostarti", IT_CLIMB_KEY, IT_CLIMB_PAD, STEER, IT_CLIMB_GYRO},
    {IT, 66, "Inclina il telecomando Wii", IT_TURN_KEY, IT_TURN_PAD, STEER, IT_TURN_GYRO},
    {IT, 80, "Al via, inclina", "Al via, spostati con [up], [left],\n[down] e [right].\nCerca di non cadere!",
     "Al via, spostati con la levetta\nsinistra.\nCerca di non cadere!", STEER,
     "Al via, inclina [it] o usa la\nlevetta sinistra per rotolare.\nCerca di non cadere!"},
    {IT, 88, "Punta il telecomando Wii", "Al via cominceranno\na volare pietre."},
    {IT, 89, "Colpisci le pietre", "Colpisci le pietre volanti con [1] o [2]!\nSe le rompi a tempo fai più punti."},
    {IT, 90, "Quando una pietra",
     "Quando una pietra ti colpisce, non\npuoi muoverti per un po'. Riprenditi\npremendo [shake] alternandoli!"},
    {IT, 95, "Punta il telecomando",
     "Sposta il puntatore con il mouse.\nPortalo oltre il bordo dello schermo\nper girarti da quella parte.",
     "Sposta il puntatore con la levetta\ndestra. Portalo oltre il bordo dello\nschermo per girarti da quella parte.", AIM,
     "Ruota [it] o usa la levetta\ndestra per spostare il puntatore. Oltre\nil bordo ti giri da quella parte."},
    {IT, 103, "Inclina il telecomando Wii", IT_TURN_KEY, IT_TURN_PAD, STEER, IT_TURN_GYRO},
    {IT, 104, "Quando vuoi spostarti", IT_CLIMB_KEY, IT_CLIMB_PAD, STEER, IT_CLIMB_GYRO},
    {IT, 111, "Scuoti il telecomando Wii", IT_RUN},
    {IT, 51, "Codacciaio",
     "[box]Codacciaio è una mossa che\npuoi usare durante le sfide.[page]\nPremendo [either shake] potrai colpire\nil Pokémon avversario."},

    /* Spanish */
    {ES, 17, "¡Contraseña incorrecta", "¡Todas las contraseñas introducidas!", NULL, PASSWORD},
#define ES_RUN "Pulsa [shake] por turnos cuando\nempieces la carrera. Cuanto más rápido\npulses, más velocidad cogerás."
    {ES, 20, "Agita el mando de Wii", ES_RUN},
    {ES, 23, "Cuando saltes",
     "Cuando saltes el último obstáculo,\npulsa [shake] por turnos con muchas\nganas hasta que cruces la meta."},
    {ES, 26, "Cuando empieces a correr", "Cuando empieces a correr, avanza\ncon [left] y [right] para evitar caerte.",
     "Cuando empieces a correr, avanza\ncon la palanca izquierda para evitar\ncaerte."},
    {ES, 31, "Cuando llegues a la cabeza",
     "Cuando llegues a la cabeza de Rayquaza,\naparecerá la mira. Muévela con\nel ratón.",
     "Cuando llegues a la cabeza de Rayquaza,\naparecerá la mira. Muévela con\nla palanca derecha.", AIM,
     "Cuando llegues a la cabeza de Rayquaza,\naparecerá la mira. Muévela girando\n[it] o con la palanca derecha."},
#define ES_SWING "Cuando empiece el tiempo, pulsa\n[shake] por turnos para balancearte."
    {ES, 35, "Sujeta el mando de Wii", ES_SWING},
    {ES, 41, "Sujeta el mando de Wii", ES_SWING},
    {ES, 46, "Agita el mando de Wii", ES_RUN},
    {ES, 48, "Al chocarte",
     "Al chocarte con los obstáculos,\nbajará tu velocidad. ¡Pulsa [shake]\npor turnos para acelerar!"},
#define ES_TURN_KEY "Para girar, pulsa [left] o [right]\nen la dirección en la que quieras virar."
#define ES_TURN_PAD "Para girar, inclina la palanca izquierda\nen la dirección en la que quieras virar."
#define ES_TURN_GYRO \
    "Para girar, inclina [it] como un\nvolante, o la palanca izquierda, en la\ndirección en la que quieras virar."
#define ES_SIDE_KEY "Pulsa [left] o [right]\ncuando quieras girar a los lados."
#define ES_SIDE_PAD "Inclina a la derecha o a la izquierda\nla palanca izquierda cuando quieras\ngirar a los lados."
#define ES_SIDE_GYRO "Inclina [it] como un volante,\no la palanca izquierda, cuando\nquieras girar a los lados."
#define ES_CLIMB_KEY "Pulsa [up] para subir\ny [down] para bajar."
#define ES_CLIMB_PAD "Inclina la palanca izquierda hacia\narriba para subir y hacia abajo\npara bajar."
#define ES_CLIMB_GYRO \
    "Inclina [it] hacia atrás\no la palanca izquierda hacia arriba para\nsubir. Haz lo contrario para bajar."
    {ES, 52, "Para girar", ES_TURN_KEY, ES_TURN_PAD, STEER, ES_TURN_GYRO},
    {ES, 59, "Inclina a la derecha", ES_SIDE_KEY, ES_SIDE_PAD, STEER, ES_SIDE_GYRO},
    {ES, 60, "Cuando quieras subir", ES_CLIMB_KEY, ES_CLIMB_PAD, STEER, ES_CLIMB_GYRO},
    {ES, 66, "Para girar", ES_TURN_KEY, ES_TURN_PAD, STEER, ES_TURN_GYRO},
    {ES, 80, "Al empezar, orienta",
     "Al empezar, desplázate con [up], [left],\n[down] y [right] sin caerte\nde la plataforma.",
     "Al empezar, desplázate con la palanca\nizquierda sin caerte de la plataforma.", STEER,
     "Al empezar, inclina [it] o usa la\npalanca izquierda para rodar sin\ncaerte de la plataforma."},
    {ES, 88, "Sujeta el mando", "Empezarán a lanzarte rocas."},
    {ES, 89, "Agita el mando de Wii para golpear",
     "Pulsa [1] o [2] para golpear las rocas y\nganar puntos. ¡Hazlo en el último\nmomento y ganarás más!"},
    {ES, 90, "Si te golpea",
     "Si te golpea una roca, no podrás\nmoverte durante unos segundos. ¡Pulsa\n[shake] por turnos para reponerte!"},
    {ES, 95, "Sitúa el mando",
     "Controla la mira con el ratón.\nLlévala más allá del borde de la\npantalla para girar hacia ese lado.",
     "Controla la mira con la palanca derecha.\nLlévala más allá del borde de la\npantalla para girar hacia ese lado.", AIM,
     "Gira [it] o usa la palanca derecha\npara controlar la mira. Más allá del\nborde, girarás hacia ese lado."},
    {ES, 103, "Cuando quieras desplazarte", ES_SIDE_KEY, ES_SIDE_PAD, STEER, ES_SIDE_GYRO},
    {ES, 104, "Cuando quieras subir", ES_CLIMB_KEY, ES_CLIMB_PAD, STEER, ES_CLIMB_GYRO},
    {ES, 111, "¡Agita el mando", ES_RUN},
    {ES, 51, "Cola Férrea",
     "[box]Cola Férrea es un movimiento que\npuedes usar en los combates.[page]\nPulsa [either shake] para propinar\nun golpe a tu oponente que lo hará\nvolar por los aires."},
};
#define TEXTS ((int)(sizeof(kText) / sizeof(kText[0])))

static uint32_t sAt[TEXTS]; /* where each is written (0: not, another language's) */
static uint32_t sGeneration = 0xFFFFFFFFu;
static int sMotion = -1; /* the motion it was written for */

static int language(void) {
    int lang = g_host_text_language ? g_host_text_language() : HOST_TEXT_EN;
    return lang >= 0 && lang < HOST_TEXTS ? lang : HOST_TEXT_EN;
}

static int motion(int* handheld) {
    *handheld = 0;
    return g_host_motion_text ? g_host_motion_text(handheld) : 0;
}

/* The next character of UTF-8 text. */
static uint32_t next_char(const char** s) {
    const uint8_t* p = (const uint8_t*)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u);
        p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = (c & 31) << 6 | (p[0] & 63u);
        p += 1;
    }
    *s = (const char*)p;
    return c;
}

static uint32_t put(uint32_t at, uint32_t end, uint16_t ch) {
    if (at + 2 <= end) ST16(at, ch);
    return at + 2;
}

static uint32_t put_str(uint32_t at, uint32_t end, const char* s) {
    while (*s) at = put(at, end, (uint16_t)next_char(&s));
    return at;
}

static uint32_t put_button(uint32_t at, uint32_t end, int glyph) {
    at = put(at, end, 0x1A);
    at = put(at, end, 0x0605); /* 6 bytes, group 5 */
    return put(at, end, (uint16_t)glyph);
}

static void button_name(uint32_t button, char* out, int n) {
    *out = 0;
    if (g_host_button_name) g_host_button_name(button, out, n);
}

/* Writes the new text, as the controls now stand, in language lang. */
static void write_all(int lang) {
    static const struct { const char* name; int glyph; } kGlyph[] = {{"[A]", 2}, {"[B]", 3}, {"[1]", 7}, {"[2]", 8}};
    static const struct { const char* name; uint32_t button; } kName[] = {
        {"[up]", HOST_PAD_SCREEN_UP}, {"[down]", HOST_PAD_SCREEN_DOWN},
        {"[left]", HOST_PAD_SCREEN_LEFT}, {"[right]", HOST_PAD_SCREEN_RIGHT},
    };
    char shake[96], either[96], one[24], two[24];
    int handheld, on = motion(&handheld);
    const char* holding = handheld ? kWords[lang].handheld : kWords[lang].controller;
    /* The generation's low bit: the controller was used last (prompts.c). */
    int pad = g_host_prompt_generation ? (int)(g_host_prompt_generation() & 1) : 0, i;
    uint32_t at = sBase, end = sBase + TEXT_SIZE;
    button_name(HOST_PAD_SHAKE, one, sizeof(one));
    button_name(HOST_PAD_SHAKE2, two, sizeof(two));
    if (*one && *two) snprintf(shake, sizeof(shake), "%s%s%s", one, kWords[lang].both, two);
    else snprintf(shake, sizeof(shake), "%s", kWords[lang].shakeKeys);
    if (*one && *two) snprintf(either, sizeof(either), "%s%s%s", one, kWords[lang].either, two);
    else snprintf(either, sizeof(either), "%s", kWords[lang].aShakeKey);
    for (i = 0; i < TEXTS; i++) {
        const char* s = pad && kText[i].pad ? kText[i].pad : kText[i].key;
        sAt[i] = 0;
        if (kText[i].lang != lang) continue;
        if (kText[i].gyro && (on & kText[i].motion)) s = kText[i].gyro;
        sAt[i] = at;
        while (*s) {
            size_t k, n = 0;
            for (k = 0; k < sizeof(kGlyph) / sizeof(kGlyph[0]); k++) {
                if (!strncmp(s, kGlyph[k].name, n = strlen(kGlyph[k].name))) break;
            }
            if (k < sizeof(kGlyph) / sizeof(kGlyph[0])) {
                at = put_button(at, end, kGlyph[k].glyph);
                s += n;
                continue;
            }
            for (k = 0; k < sizeof(kName) / sizeof(kName[0]); k++) {
                if (!strncmp(s, kName[k].name, n = strlen(kName[k].name))) break;
            }
            if (k < sizeof(kName) / sizeof(kName[0])) {
                char name[24];
                button_name(kName[k].button, name, sizeof(name));
                at = put_str(at, end, *name ? name : kWords[lang].nothing);
                s += n;
            } else if (!strncmp(s, "[shake]", 7)) {
                at = put_str(at, end, shake);
                s += 7;
            } else if (!strncmp(s, "[either shake]", 14)) {
                at = put_str(at, end, either);
                s += 14;
            } else if (!strncmp(s, "[it]", 4)) {
                at = put_str(at, end, holding);
                s += 4;
            } else if (!strncmp(s, "[box]", 5)) {
                at = put(at, end, 0x1A);
                at = put(at, end, 0x08FF);
                at = put(at, end, 0x0000);
                at = put(at, end, 0x0400);
                s += 5;
            } else if (!strncmp(s, "[page]", 6)) {
                at = put(at, end, 0x1A);
                at = put(at, end, 0x0803);
                at = put(at, end, 0x0000);
                at = put(at, end, 0x0100);
                s += 6;
            } else {
                at = put(at, end, (uint16_t)next_char(&s));
            }
        }
        at = put(at, end, 0);
        if (at > end) {
            HLE_WARN("text: no room for the minigame instructions");
            sAt[i] = 0;
            at = end;
        }
    }
}

/* Whether the string at s (UTF-16) begins with prefix (UTF-8), after a
 * tag if it begins with one. */
static int begins(uint32_t s, const char* prefix) {
    if (LD16(s) == 0x1A) s += LD8(s + 2);
    while (*prefix) {
        if (LD16(s) != next_char(&prefix)) return 0;
        s += 2;
    }
    return 1;
}

/* const wchar_t* MessageData::GetString(int index): message index of a
 * BMG file (the object holds its INF1 at +8, DAT1 at +0xC). */
void f_801081C0(ppc_t* c) {
    int index = (int)c->r[4], i, on, handheld, lang;
    uint32_t gen;
    f_801081C0_orig(c);
    if (!sBase || !c->r[3]) return;
    lang = language();
    for (i = 0; i < TEXTS; i++) {
        if (kText[i].lang != lang || kText[i].index != index || !begins(c->r[3], kText[i].was)) continue;
        if ((kText[i].motion & PASSWORD) && !hle_password_unlock_saying()) return;
        gen = g_host_prompt_generation ? g_host_prompt_generation() : 0;
        on = motion(&handheld);
        if (gen != sGeneration || on != sMotion) {
            sGeneration = gen;
            sMotion = on;
            write_all(lang);
        }
        if (sAt[i]) c->r[3] = sAt[i];
        return;
    }
}
