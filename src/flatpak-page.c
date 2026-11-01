#define _GNU_SOURCE
#include "flatpak-page.h"
#include "debug-log.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;

#define FLATPAK_BIN "flatpak"
#define FLATPAK_REMOTE "flathub"
#define FLATPAK_REMOTE_URL "https://flathub.org/repo/flathub.flatpakrepo"
#define EAPT_DIR "/usr/lib/eapt"
#define EAPT_CATALOG EAPT_DIR "/flatpak-cache-v2.tsv"
#define EAPT_ICON_INDEX EAPT_DIR "/flatpak-icon-cache.tsv"
#define FLATPAK_FALLBACK_ICON "/usr/lib/edpkg-gui/essora-flatpak-64.png"
#define FP_INSTALLED_ICON "/usr/lib/edpkg-gui/package-installed.png"
#define FP_UPGRADABLE_ICON "/usr/lib/edpkg-gui/package-upgradable.png"
#define FLATPAK_EXPORT_DESKTOP "/var/lib/flatpak/exports/share/applications"
#define FLATPAK_EXPORT_ICONS "/var/lib/flatpak/exports/share/icons"
#define FLATPAK_TMPDIR "/var/tmp/flatpak-temp"
#define FIXMENUS_BIN "/usr/bin/fixmenus"
#define FIXMENU_COMPAT_BIN "/usr/local/bin/fixmenu"

typedef enum {
    FP_FILTER_ALL,
    FP_FILTER_INSTALLED,
    FP_FILTER_UPGRADABLE,
    FP_FILTER_NOT_INSTALLED
} FpFilter;

typedef enum {
    FP_ACT_NONE,
    FP_ACT_INSTALL,
    FP_ACT_REMOVE,
    FP_ACT_REINSTALL,
    FP_ACT_UPGRADE
} FpAction;

typedef struct {
    char *id;
    char *name;
    char *description;
    char *remote;
    char *installed_version;
    char *available_version;
    char *search_key;
    gboolean installed;
    gboolean upgradable;
    FpAction action;
} FpApp;

enum {
    FP_C_ACTION,
    FP_C_ICON,
    FP_C_NAME,
    FP_C_ID,
    FP_C_INSTALLED,
    FP_C_AVAILABLE,
    FP_C_BG,
    FP_C_FG,
    FP_C_HAS_COLOR,
    FP_C_STATE_ICON, /* shared DEB status indicator, separate from application artwork */
    FP_N_COLS
};

typedef struct {
    GtkWindow *parent;
    GtkWidget *root;
    GtkWidget *tree;
    GtkListStore *store;
    GPtrArray *sorted_apps;
    GHashTable *pixbuf_cache;
    GtkWidget *search;
    GtkWidget *status;
    GtkWidget *repo_notice;
    GtkWidget *detail_name;
    GtkWidget *detail_desc;
    GtkWidget *detail_versions;
    GtkWidget *btn_refresh;
    GtkWidget *btn_mark_upgrades;
    GtkWidget *btn_apply;
    GtkWidget *btn_install;
    GtkWidget *btn_remove;
    GtkWidget *btn_reinstall;
    GtkWidget *btn_clear;
    GtkWidget *filter_buttons[4];
    GtkWidget *progress_dialog;
    GtkWidget *progress_status;
    GtkWidget *progress_bar;
    GtkWidget *progress_text;
    GtkTextBuffer *progress_buffer;
    GtkWidget *progress_close;
    GtkWidget *progress_scroller;
    GMutex progress_lock;
    GString *progress_pending;
    gchar *progress_pending_status;
    guint progress_flush_id;
    GHashTable *apps;
    GHashTable *icon_index;
    GdkPixbuf *fallback_icon;
    GdkPixbuf *icon_installed;
    GdkPixbuf *icon_upgradable;
    FpFilter filter_mode;
    gboolean is_root;
    gboolean busy;
    gboolean catalog_loaded;
    gboolean repo_refreshed; /* successful explicit refresh in current session */
    char *search_folded;
    guint search_debounce_id;
    guint pulse_id;
} FpPage;

typedef enum {
    FT_REFRESH,
    FT_MARK_UPGRADES,
    FT_APPLY,
    FT_SEARCH,
    FT_ALL,
    FT_INSTALLED,
    FT_UPGRADABLE,
    FT_NOT_INSTALLED,
    FT_ACTION,
    FT_APP,
    FT_APP_ID,
    FT_INSTALLED_VER,
    FT_AVAILABLE_VER,
    FT_DESCRIPTION,
    FT_MARK_INSTALL,
    FT_MARK_REMOVE,
    FT_MARK_REINSTALL,
    FT_CLEAR_MARK,
    FT_READY,
    FT_NO_SELECTION,
    FT_RUNNING,
    FT_DONE,
    FT_FAILED,
    FT_NO_MARKS,
    FT_CONFIRM_APPLY,
    FT_ROOT_REQUIRED,
    FT_ROOT_REQUIRED_TEXT,
    FT_NOT_INSTALLED_TOOL,
    FT_PREPARING,
    FT_REFRESHING,
    FT_OFFLINE_NO_CACHE,
    FT_COUNT_FMT,
    FT_SELECTED_FMT,
    FT_MULTI_HELP,
    FT_REPO_NOTICE,
    FT_COUNT
} FpTrKey;

typedef struct { const char *code; const char *s[FT_COUNT]; } FpLang;

static const FpLang fp_langs[] = {
 {"en", {
  "Refresh", "Mark upgrades", "Apply", "Search Flatpak apps…", "All", "Installed", "Upgradable", "Not installed",
  "Action", "Application", "Application ID", "Installed", "Available", "Description", "Mark install", "Mark remove",
  "Mark reinstall", "Clear mark", "Ready", "Select an application to see details", "Running…", "Completed", "Operation failed",
  "There are no marked changes.", "Apply the marked Flatpak changes?", "Administrator privileges required",
  "Flatpak changes and catalog refresh require root privileges. Browsing remains available.", "Flatpak is not installed.",
  "Preparing Flatpak catalog…", "Updating Flatpak repositories…", "No Flatpak catalog is cached. Connect to the Internet and use Refresh.", "%u applications",
  "%u applications selected", "Ctrl-click selects individual applications; Shift-click selects a range.",
  "Installed Flatpak applications are shown. Refresh repositories before installing or upgrading applications."
 }},
 {"es", {
  "Actualizar", "Marcar actualizaciones", "Aplicar", "Buscar aplicaciones Flatpak…", "Todas", "Instaladas", "Actualizables", "No instaladas",
  "Acción", "Aplicación", "ID de aplicación", "Instalada", "Disponible", "Descripción", "Marcar instalar", "Marcar eliminar",
  "Marcar reinstalar", "Quitar marca", "Listo", "Selecciona una aplicación para ver detalles", "Ejecutando…", "Completado", "La operación falló",
  "No hay cambios marcados.", "¿Aplicar los cambios Flatpak marcados?", "Se requieren privilegios de administrador",
  "Los cambios Flatpak y la actualización del catálogo requieren root. La consulta seguirá disponible.", "Flatpak no está instalado.",
  "Preparando catálogo Flatpak…", "Se están actualizando los repositorios Flatpak…", "No hay catálogo Flatpak en caché. Conéctate a Internet y usa Actualizar.", "%u aplicaciones",
  "%u aplicaciones seleccionadas", "Ctrl+clic selecciona aplicaciones individuales; Mayús+clic selecciona un rango.",
  "Se muestran las aplicaciones Flatpak instaladas. Actualiza los repositorios antes de instalar o actualizar aplicaciones."
 }},
 {"fr", {
  "Actualiser", "Marquer les mises à jour", "Appliquer", "Rechercher des applications Flatpak…", "Toutes", "Installées", "Mises à jour", "Non installées",
  "Action", "Application", "ID d’application", "Installée", "Disponible", "Description", "Marquer installation", "Marquer suppression",
  "Marquer réinstallation", "Effacer la marque", "Prêt", "Sélectionnez une application", "Exécution…", "Terminé", "Échec de l’opération",
  "Aucun changement marqué.", "Appliquer les changements Flatpak marqués ?", "Privilèges administrateur requis",
  "Les changements Flatpak et l’actualisation du catalogue nécessitent root. La consultation reste disponible.", "Flatpak n’est pas installé.",
  "Préparation du catalogue Flatpak…", "Mise à jour des dépôts Flatpak en cours…", "Aucun catalogue Flatpak en cache. Connectez-vous à Internet puis actualisez.", "%u applications",
  "%u applications sélectionnées", "Ctrl+clic sélectionne des applications ; Maj+clic sélectionne une plage.",
  "Les applications Flatpak installées sont affichées. Actualisez les dépôts avant toute installation ou mise à niveau."
 }},
 {"it", {
  "Aggiorna", "Segna aggiornamenti", "Applica", "Cerca applicazioni Flatpak…", "Tutte", "Installate", "Aggiornabili", "Non installate",
  "Azione", "Applicazione", "ID applicazione", "Installata", "Disponibile", "Descrizione", "Segna installazione", "Segna rimozione",
  "Segna reinstallazione", "Rimuovi segno", "Pronto", "Seleziona un’applicazione", "In esecuzione…", "Completato", "Operazione fallita",
  "Non ci sono modifiche segnate.", "Applicare le modifiche Flatpak segnate?", "Servono privilegi di amministratore",
  "Le modifiche Flatpak e l’aggiornamento del catalogo richiedono root. La consultazione resta disponibile.", "Flatpak non è installato.",
  "Preparazione catalogo Flatpak…", "Aggiornamento dei repository Flatpak in corso…", "Nessun catalogo Flatpak in cache. Connettiti a Internet e aggiorna.", "%u applicazioni",
  "%u applicazioni selezionate", "Ctrl+clic seleziona singole applicazioni; Maiusc+clic seleziona un intervallo.",
  "Sono mostrate le applicazioni Flatpak installate. Aggiorna i repository prima di installare o aggiornare le applicazioni."
 }},
 {"pt", {
  "Atualizar", "Marcar atualizações", "Aplicar", "Pesquisar aplicativos Flatpak…", "Todos", "Instalados", "Atualizáveis", "Não instalados",
  "Ação", "Aplicativo", "ID do aplicativo", "Instalada", "Disponível", "Descrição", "Marcar instalar", "Marcar remover",
  "Marcar reinstalar", "Limpar marca", "Pronto", "Selecione um aplicativo", "Executando…", "Concluído", "A operação falhou",
  "Não há alterações marcadas.", "Aplicar as alterações Flatpak marcadas?", "Privilégios de administrador necessários",
  "Alterações Flatpak e atualização do catálogo exigem root. A consulta continua disponível.", "Flatpak não está instalado.",
  "Preparando catálogo Flatpak…", "Atualizando os repositórios Flatpak…", "Não há catálogo Flatpak em cache. Conecte-se à Internet e atualize.", "%u aplicativos",
  "%u aplicativos selecionados", "Ctrl+clique seleciona aplicativos individuais; Shift+clique seleciona um intervalo.",
  "Os aplicativos Flatpak instalados são exibidos. Atualize os repositórios antes de instalar ou atualizar aplicativos."
 }},
 {"ca", {
  "Actualitza", "Marca actualitzacions", "Aplica", "Cerca aplicacions Flatpak…", "Totes", "Instal·lades", "Actualitzables", "No instal·lades",
  "Acció", "Aplicació", "ID d’aplicació", "Instal·lada", "Disponible", "Descripció", "Marca instal·lació", "Marca eliminació",
  "Marca reinstal·lació", "Esborra marca", "Preparat", "Selecciona una aplicació", "Executant…", "Completat", "L’operació ha fallat",
  "No hi ha canvis marcats.", "Aplicar els canvis Flatpak marcats?", "Calen privilegis d’administrador",
  "Els canvis Flatpak i l’actualització del catàleg requereixen root. La consulta continua disponible.", "Flatpak no està instal·lat.",
  "Preparant el catàleg Flatpak…", "S’estan actualitzant els repositoris Flatpak…", "No hi ha catàleg Flatpak en memòria cau. Connecta’t a Internet i actualitza.", "%u aplicacions",
  "%u aplicacions seleccionades", "Ctrl+clic selecciona aplicacions individuals; Maj+clic selecciona un interval.",
  "Es mostren les aplicacions Flatpak instal·lades. Actualitza els repositoris abans d’instal·lar o actualitzar aplicacions."
 }},
 {"de", {
  "Aktualisieren", "Aktualisierungen markieren", "Anwenden", "Flatpak-Apps suchen…", "Alle", "Installiert", "Aktualisierbar", "Nicht installiert",
  "Aktion", "Anwendung", "Anwendungs-ID", "Installiert", "Verfügbar", "Beschreibung", "Installation markieren", "Entfernen markieren",
  "Neuinstallation markieren", "Markierung löschen", "Bereit", "Anwendung für Details auswählen", "Wird ausgeführt…", "Abgeschlossen", "Vorgang fehlgeschlagen",
  "Es sind keine Änderungen markiert.", "Markierte Flatpak-Änderungen anwenden?", "Administratorrechte erforderlich",
  "Flatpak-Änderungen und Katalogaktualisierung benötigen root. Durchsuchen bleibt möglich.", "Flatpak ist nicht installiert.",
  "Flatpak-Katalog wird vorbereitet…", "Flatpak-Paketquellen werden aktualisiert…", "Kein Flatpak-Katalog im Cache. Mit dem Internet verbinden und aktualisieren.", "%u Anwendungen",
  "%u Anwendungen ausgewählt", "Strg+Klick wählt einzelne Anwendungen; Umschalt+Klick einen Bereich.",
  "Installierte Flatpak-Anwendungen werden angezeigt. Aktualisieren Sie die Paketquellen vor der Installation oder Aktualisierung."
 }},
 {"hu", {
  "Frissítés", "Frissítések kijelölése", "Alkalmaz", "Flatpak alkalmazások keresése…", "Mind", "Telepített", "Frissíthető", "Nincs telepítve",
  "Művelet", "Alkalmazás", "Alkalmazásazonosító", "Telepített", "Elérhető", "Leírás", "Telepítés kijelölése", "Eltávolítás kijelölése",
  "Újratelepítés kijelölése", "Jelölés törlése", "Kész", "Válassz alkalmazást a részletekhez", "Futtatás…", "Kész", "A művelet sikertelen",
  "Nincs kijelölt módosítás.", "Alkalmazza a kijelölt Flatpak módosításokat?", "Rendszergazdai jogosultság szükséges",
  "A Flatpak módosításokhoz és a katalógus frissítéséhez root szükséges. A böngészés továbbra is elérhető.", "A Flatpak nincs telepítve.",
  "Flatpak katalógus előkészítése…", "Flatpak tárolók frissítése folyamatban…", "Nincs gyorsítótárazott Flatpak katalógus. Csatlakozz az internethez és frissíts.", "%u alkalmazás",
  "%u alkalmazás kijelölve", "Ctrl+kattintás egyenként, Shift+kattintás tartományt jelöl ki.",
  "A telepített Flatpak alkalmazások láthatók. Telepítés vagy frissítés előtt frissítse a tárolókat."
 }},
 {"ru", {
  "Обновить", "Отметить обновления", "Применить", "Поиск приложений Flatpak…", "Все", "Установленные", "Обновляемые", "Не установленные",
  "Действие", "Приложение", "ID приложения", "Установлена", "Доступна", "Описание", "Отметить установку", "Отметить удаление",
  "Отметить переустановку", "Снять отметку", "Готово", "Выберите приложение", "Выполняется…", "Завершено", "Ошибка операции",
  "Нет отмеченных изменений.", "Применить отмеченные изменения Flatpak?", "Требуются права администратора",
  "Изменения Flatpak и обновление каталога требуют root. Просмотр остаётся доступным.", "Flatpak не установлен.",
  "Подготовка каталога Flatpak…", "Идёт обновление репозиториев Flatpak…", "Нет кэша каталога Flatpak. Подключитесь к Интернету и обновите.", "%u приложений",
  "Выбрано приложений: %u", "Ctrl+щелчок выбирает отдельные приложения; Shift+щелчок — диапазон.",
  "Показаны установленные приложения Flatpak. Обновите репозитории перед установкой или обновлением приложений."
 }},
 {"ja", {
  "更新", "更新をマーク", "適用", "Flatpak アプリを検索…", "すべて", "インストール済み", "更新可能", "未インストール",
  "操作", "アプリケーション", "アプリケーション ID", "インストール済み", "利用可能", "説明", "インストールをマーク", "削除をマーク",
  "再インストールをマーク", "マークを解除", "準備完了", "詳細を見るアプリを選択", "実行中…", "完了", "操作に失敗しました",
  "マークされた変更はありません。", "マークした Flatpak の変更を適用しますか？", "管理者権限が必要です",
  "Flatpak の変更とカタログ更新には root 権限が必要です。閲覧は可能です。", "Flatpak がインストールされていません。",
  "Flatpak カタログを準備中…", "Flatpak リポジトリを更新しています…", "Flatpak カタログのキャッシュがありません。インターネット接続後に更新してください。", "%u 個のアプリ",
  "%u 個のアプリを選択", "Ctrl+クリックで個別選択、Shift+クリックで範囲選択します。",
  "インストール済み Flatpak アプリを表示しています。インストールや更新の前にリポジトリを更新してください。"
 }},
 {"zh", {
  "刷新", "标记更新", "应用", "搜索 Flatpak 应用…", "全部", "已安装", "可更新", "未安装",
  "操作", "应用程序", "应用程序 ID", "已安装", "可用", "说明", "标记安装", "标记删除",
  "标记重新安装", "清除标记", "就绪", "选择应用以查看详情", "正在运行…", "已完成", "操作失败",
  "没有已标记的更改。", "应用已标记的 Flatpak 更改？", "需要管理员权限",
  "Flatpak 更改和目录刷新需要 root 权限。仍可浏览。", "未安装 Flatpak。",
  "正在准备 Flatpak 目录…", "正在更新 Flatpak 软件源…", "没有 Flatpak 目录缓存。请联网后刷新。", "%u 个应用",
  "已选择 %u 个应用", "Ctrl+单击选择单个应用；Shift+单击选择范围。",
  "当前显示已安装的 Flatpak 应用。安装或更新应用前，请先刷新软件源。"
 }},
 {"ar", {
  "تحديث", "تحديد التحديثات", "تطبيق", "البحث عن تطبيقات Flatpak…", "الكل", "مثبت", "قابل للتحديث", "غير مثبت",
  "الإجراء", "التطبيق", "معرّف التطبيق", "المثبت", "المتاح", "الوصف", "تحديد للتثبيت", "تحديد للإزالة",
  "تحديد لإعادة التثبيت", "مسح التحديد", "جاهز", "اختر تطبيقاً لعرض التفاصيل", "جارٍ التنفيذ…", "اكتمل", "فشلت العملية",
  "لا توجد تغييرات محددة.", "تطبيق تغييرات Flatpak المحددة؟", "صلاحيات المدير مطلوبة",
  "تغييرات Flatpak وتحديث الفهرس تحتاج صلاحيات root. يبقى التصفح متاحاً.", "Flatpak غير مثبت.",
  "جارٍ تجهيز فهرس Flatpak…", "جارٍ تحديث مستودعات Flatpak…", "لا توجد ذاكرة مؤقتة لفهرس Flatpak. اتصل بالإنترنت ثم حدّث.", "%u تطبيقات",
  "تم تحديد %u تطبيقات", "Ctrl+نقرة لاختيار تطبيقات منفردة وShift+نقرة لاختيار نطاق.",
  "يتم عرض تطبيقات Flatpak المثبتة. حدّث المستودعات قبل تثبيت التطبيقات أو ترقيتها."
 }}
};

static const FpLang *FP_L = &fp_langs[0];
static const char *ftr(FpTrKey k) { return FP_L->s[k]; }

static gchar *fp_format_count(FpTrKey key, guint count) {
    const gchar *fmt = ftr(key);
    const gchar *mark = strstr(fmt, "%u");
    if (!mark) return g_strdup(fmt);
    gchar *prefix = g_strndup(fmt, (gsize)(mark - fmt));
    gchar *number = g_strdup_printf("%u", count);
    gchar *result = g_strconcat(prefix, number, mark + 2, NULL);
    g_free(prefix);
    g_free(number);
    return result;
}

static void fp_choose_language(void) {
    const char *e = g_getenv("LANG");
    char code[8] = "en";
    if (e && *e) {
        size_t n = strcspn(e, "_.@");
        if (n > 0 && n < sizeof(code)) {
            memcpy(code, e, n);
            code[n] = 0;
            for (size_t i = 0; code[i]; i++) code[i] = (char)g_ascii_tolower(code[i]);
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS(fp_langs); i++) {
        if (!strcmp(code, fp_langs[i].code)) { FP_L = &fp_langs[i]; break; }
    }
}

static void fp_app_free(gpointer ptr) {
    FpApp *a = ptr;
    if (!a) return;
    g_free(a->id);
    g_free(a->name);
    g_free(a->description);
    g_free(a->remote);
    g_free(a->installed_version);
    g_free(a->available_version);
    g_free(a->search_key);
    g_free(a);
}

static FpApp *fp_app_new(const char *id) {
    FpApp *a = g_new0(FpApp, 1);
    a->id = g_strdup(id ? id : "");
    a->name = g_strdup(id ? id : "");
    a->description = g_strdup("");
    a->remote = g_strdup(FLATPAK_REMOTE);
    a->installed_version = g_strdup("");
    a->available_version = g_strdup("");
    a->search_key = g_strdup("");
    return a;
}

static void fp_set_string(char **dst, const char *src) {
    g_free(*dst);
    *dst = g_strdup(src ? src : "");
}

static void fp_refresh_search_key(FpApp *a) {
    char *raw = g_strconcat(a->name ? a->name : "", "\n", a->id ? a->id : "", "\n",
                            a->description ? a->description : "", NULL);
    char *low = g_utf8_strdown(raw, -1);
    g_free(raw);
    fp_set_string(&a->search_key, low);
    g_free(low);
}

static GHashTable *fp_apps_new(void) {
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fp_app_free);
}

static FpApp *fp_get_or_create(GHashTable *apps, const char *id) {
    FpApp *a = g_hash_table_lookup(apps, id);
    if (a) return a;
    a = fp_app_new(id);
    g_hash_table_insert(apps, g_strdup(id), a);
    return a;
}

static char **fp_env(void) {
    g_mkdir_with_parents(FLATPAK_TMPDIR, 0700);
    int n = 0;
    while (environ[n]) n++;
    char **env = g_new0(char *, n + 2);
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (!g_str_has_prefix(environ[i], "TMPDIR=")) env[j++] = g_strdup(environ[i]);
    }
    env[j++] = g_strdup("TMPDIR=" FLATPAK_TMPDIR);
    env[j] = NULL;
    return env;
}

static void fp_free_env(char **env) {
    if (!env) return;
    for (int i = 0; env[i]; i++) g_free(env[i]);
    g_free(env);
}

static gchar *fp_capture(gchar *argv[], gint *out_status) {
    gchar *out = NULL, *err = NULL;
    GError *e = NULL;
    gint status = -1;
    char **env = fp_env();
    gboolean ok = g_spawn_sync(NULL, argv, env, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, &out, &err, &status, &e);
    fp_free_env(env);
    if (!ok) {
        if (out_status) *out_status = -1;
        gchar *msg = g_strdup(e ? e->message : "spawn failed");
        if (e) g_error_free(e);
        g_free(out);
        g_free(err);
        return msg;
    }
    if (out_status) {
        if (WIFEXITED(status)) *out_status = WEXITSTATUS(status);
        else *out_status = -1;
    }
    if (err && *err) {
        if (out && *out) {
            gchar *both = g_strconcat(out, "\n", err, NULL);
            g_free(out);
            out = both;
        } else {
            g_free(out);
            out = g_strdup(err);
        }
    }
    g_free(err);
    return out ? out : g_strdup("");
}

static gboolean fp_program_exists(void) {
    gchar *p = g_find_program_in_path(FLATPAK_BIN);
    gboolean ok = p != NULL;
    g_free(p);
    return ok;
}

static gboolean fp_remote_exists(void) {
    char *argv[] = { FLATPAK_BIN, "--system", "remotes", "--columns=name", NULL };
    gint rc = -1;
    gchar *out = fp_capture(argv, &rc);
    gboolean found = FALSE;
    if (rc == 0) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar *s = g_strstrip(lines[i]);
            if (!strcmp(s, FLATPAK_REMOTE)) { found = TRUE; break; }
        }
        g_strfreev(lines);
    }
    g_free(out);
    return found;
}

static gboolean fp_internet_available(void) {
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 3);
    GError *e = NULL;
    GSocketConnection *conn = g_socket_client_connect_to_host(client, "flathub.org", 443, NULL, &e);
    if (conn) g_object_unref(conn);
    if (e) g_error_free(e);
    g_object_unref(client);
    return conn != NULL;
}

static gboolean fp_ensure_remote(void) {
    if (fp_remote_exists()) return TRUE;
    if (!fp_internet_available()) return FALSE;
    char *argv[] = { FLATPAK_BIN, "--system", "remote-add", "--if-not-exists",
                     FLATPAK_REMOTE, FLATPAK_REMOTE_URL, NULL };
    gint rc = -1;
    gchar *out = fp_capture(argv, &rc);
    g_free(out);
    return rc == 0 && fp_remote_exists();
}

static gchar *fp_clean_field(const char *s) {
    gchar *x = g_strdup(s ? s : "");
    for (char *p = x; *p; p++) {
        if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
    }
    return x;
}

static gint fp_cache_sort(gconstpointer aa, gconstpointer bb) {
    const FpApp *a = aa, *b = bb;
    return g_utf8_collate(a->name ? a->name : a->id, b->name ? b->name : b->id);
}

static gboolean fp_save_cache(GHashTable *apps) {
    if (g_mkdir_with_parents(EAPT_DIR, 0755) != 0) return FALSE;
    GList *values = g_hash_table_get_values(apps);
    values = g_list_sort(values, fp_cache_sort);
    GString *out = g_string_new("# edpkg-gui-flatpak-cache-v2\n");
    for (GList *l = values; l; l = l->next) {
        FpApp *a = l->data;
        gchar *id = fp_clean_field(a->id), *name = fp_clean_field(a->name);
        gchar *desc = fp_clean_field(a->description), *remote = fp_clean_field(a->remote);
        gchar *inst = fp_clean_field(a->installed_version), *avail = fp_clean_field(a->available_version);
        g_string_append_printf(out, "%d\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
                               a->installed ? 1 : 0, id, name, desc, remote,
                               inst, avail, a->upgradable ? 1 : 0);
        g_free(id); g_free(name); g_free(desc); g_free(remote); g_free(inst); g_free(avail);
    }
    g_list_free(values);
    gboolean ok = g_file_set_contents(EAPT_CATALOG, out->str, out->len, NULL);
    g_string_free(out, TRUE);
    return ok;
}

static GHashTable *fp_load_cache(void) {
    gchar *text = NULL;
    if (!g_file_get_contents(EAPT_CATALOG, &text, NULL, NULL)) return NULL;
    gchar **lines = g_strsplit(text, "\n", -1);
    if (!lines[0] || strcmp(lines[0], "# edpkg-gui-flatpak-cache-v2")) {
        g_strfreev(lines); g_free(text); return NULL;
    }
    GHashTable *apps = fp_apps_new();
    for (int i = 1; lines[i]; i++) {
        if (!*lines[i]) continue;
        gchar **f = g_strsplit(lines[i], "\t", 8);
        if (g_strv_length(f) >= 8 && f[1] && *f[1]) {
            FpApp *a = fp_get_or_create(apps, f[1]);
            a->installed = atoi(f[0]) != 0;
            fp_set_string(&a->name, (f[2] && *f[2]) ? f[2] : f[1]);
            fp_set_string(&a->description, f[3]);
            fp_set_string(&a->remote, (f[4] && *f[4]) ? f[4] : FLATPAK_REMOTE);
            fp_set_string(&a->installed_version, f[5]);
            fp_set_string(&a->available_version, f[6]);
            a->upgradable = atoi(f[7]) != 0;
            fp_refresh_search_key(a);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(text);
    return apps;
}

static gboolean fp_parse_remote_catalog(GHashTable *apps, gchar **error_text) {
    char *argv[] = { FLATPAK_BIN, "--system", "remote-ls", FLATPAK_REMOTE, "--app",
                     "--columns=application,name,description,version,branch", NULL };
    gint rc = -1;
    gchar *out = fp_capture(argv, &rc);
    if (rc != 0) {
        if (error_text) *error_text = g_strdup(out);
        g_free(out);
        return FALSE;
    }
    gchar **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        gchar **f = g_strsplit(lines[i], "\t", 5);
        guint n = g_strv_length(f);
        if (n >= 2 && f[0] && *f[0]) {
            FpApp *a = fp_get_or_create(apps, g_strstrip(f[0]));
            if (f[1] && *g_strstrip(f[1])) fp_set_string(&a->name, g_strstrip(f[1]));
            if (n >= 3) fp_set_string(&a->description, g_strstrip(f[2]));
            const char *ver = (n >= 4 && *g_strstrip(f[3])) ? g_strstrip(f[3]) :
                              (n >= 5 ? g_strstrip(f[4]) : "");
            fp_set_string(&a->available_version, ver);
            fp_set_string(&a->remote, FLATPAK_REMOTE);
            a->upgradable = FALSE;
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(out);

    char *uargv[] = { FLATPAK_BIN, "--system", "remote-ls", "--updates", "--app",
                      "--columns=application,version,branch", NULL };
    out = fp_capture(uargv, &rc);
    if (rc == 0) {
        lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i]) continue;
            gchar **f = g_strsplit(lines[i], "\t", 3);
            if (f[0] && *g_strstrip(f[0])) {
                FpApp *a = fp_get_or_create(apps, g_strstrip(f[0]));
                a->upgradable = TRUE;
                const char *ver = (f[1] && *g_strstrip(f[1])) ? g_strstrip(f[1]) :
                                  (f[2] ? g_strstrip(f[2]) : "");
                if (*ver) fp_set_string(&a->available_version, ver);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return TRUE;
}

static void fp_overlay_installed(GHashTable *apps) {
    GHashTableIter hit;
    gpointer key, value;
    g_hash_table_iter_init(&hit, apps);
    while (g_hash_table_iter_next(&hit, &key, &value)) {
        FpApp *a = value;
        a->installed = FALSE;
        fp_set_string(&a->installed_version, "");
    }

    char *argv[] = { FLATPAK_BIN, "--system", "list", "--app",
                     "--columns=application,version,branch,name", NULL };
    gint rc = -1;
    gchar *out = fp_capture(argv, &rc);
    if (rc == 0) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i]) continue;
            gchar **f = g_strsplit(lines[i], "\t", 4);
            guint n = g_strv_length(f);
            if (n >= 1 && f[0] && *g_strstrip(f[0])) {
                const char *id = g_strstrip(f[0]);
                FpApp *a = fp_get_or_create(apps, id);
                a->installed = TRUE;
                const char *ver = (n >= 2 && *g_strstrip(f[1])) ? g_strstrip(f[1]) :
                                  (n >= 3 ? g_strstrip(f[2]) : "");
                fp_set_string(&a->installed_version, ver);
                if (n >= 4 && f[3] && *g_strstrip(f[3]) && !strcmp(a->name, a->id))
                    fp_set_string(&a->name, g_strstrip(f[3]));
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(out);

    g_hash_table_iter_init(&hit, apps);
    while (g_hash_table_iter_next(&hit, &key, &value)) fp_refresh_search_key(value);
}

/* Refresh Flathub AppStream metadata only during first cache creation or
 * when the user presses Refresh in the Flatpak tab.  Normal startup never
 * reaches this path once the persistent catalog exists. */
static void fp_refresh_appstream(void) {
    char *argv[] = { FLATPAK_BIN, "--system", "update", "--appstream", FLATPAK_REMOTE, NULL };
    gint rc = -1;
    gchar *out = fp_capture(argv, &rc);
    g_free(out);
}

static GHashTable *fp_build_remote_catalog(gboolean allow_remote_add, gchar **error_text) {
    if (error_text) *error_text = NULL;

    if (!fp_remote_exists()) {
        if (!allow_remote_add || !fp_ensure_remote()) {
            if (error_text) *error_text = g_strdup(ftr(FT_OFFLINE_NO_CACHE));
            return NULL;
        }
    }

    /* Store-like refresh: update repository metadata, then rebuild the complete
     * Flatpak catalog (available + installed + upgrades) and persist it. */
    fp_refresh_appstream();

    GHashTable *apps = fp_apps_new();
    gchar *parse_error = NULL;
    if (!fp_parse_remote_catalog(apps, &parse_error)) {
        if (error_text) *error_text = parse_error;
        else g_free(parse_error);
        g_hash_table_destroy(apps);
        return NULL;
    }

    fp_overlay_installed(apps);
    fp_save_cache(apps);
    return apps;
}

static GHashTable *fp_icon_index_new(void) {
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static gboolean fp_icon_file_supported(const char *name) {
    if (!name) return FALSE;
    const char *dot = strrchr(name, '.');
    if (!dot) return FALSE;
    return !g_ascii_strcasecmp(dot, ".png") || !g_ascii_strcasecmp(dot, ".svg") ||
           !g_ascii_strcasecmp(dot, ".jpg") || !g_ascii_strcasecmp(dot, ".jpeg") ||
           !g_ascii_strcasecmp(dot, ".webp") || !g_ascii_strcasecmp(dot, ".xpm");
}

static gchar *fp_icon_key_from_file(const char *name) {
    gchar *key = g_strdup(name ? name : "");
    char *dot = strrchr(key, '.');
    if (dot) *dot = '\0';
    return key;
}

static gint fp_icon_path_score(const char *path) {
    if (!path) return 0;
    if (strstr(path, "/128x128/")) return 100;
    if (strstr(path, "/256x256/")) return 95;
    if (strstr(path, "/64x64/")) return 90;
    if (strstr(path, "/48x48/")) return 85;
    if (strstr(path, "/512x512/")) return 80;
    if (strstr(path, "/scalable/")) return 75;
    if (strstr(path, "/32x32/")) return 70;
    return 10;
}

/* Same idea as Essora Store's icon-cache: scan the Flatpak export icon tree
 * once and remember icon-name -> real exported path.  This function is only
 * called when the cache does not exist yet or when the user presses Refresh. */
static void fp_scan_icon_tree(GHashTable *index, const char *dir, guint depth) {
    if (!index || !dir || depth > 8) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        gchar *path = g_build_filename(dir, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            fp_scan_icon_tree(index, path, depth + 1);
            g_free(path);
            continue;
        }
        if (!g_file_test(path, G_FILE_TEST_IS_REGULAR) || !fp_icon_file_supported(name)) {
            g_free(path);
            continue;
        }
        gchar *key = fp_icon_key_from_file(name);
        const char *old = g_hash_table_lookup(index, key);
        if (!old || fp_icon_path_score(path) > fp_icon_path_score(old))
            g_hash_table_replace(index, g_strdup(key), g_strdup(path));
        g_free(key);
        g_free(path);
    }
    g_dir_close(d);
}

/* Installed Flatpaks sometimes use an Icon= name different from their app ID.
 * Add app-id aliases while building the cache so normal GUI startup never has
 * to open every .desktop file again. */
static void fp_add_desktop_icon_aliases(GHashTable *index) {
    GDir *d = g_dir_open(FLATPAK_EXPORT_DESKTOP, 0, NULL);
    if (!d) return;
    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (!g_str_has_suffix(name, ".desktop")) continue;
        gchar *desktop_path = g_build_filename(FLATPAK_EXPORT_DESKTOP, name, NULL);
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, desktop_path, G_KEY_FILE_NONE, NULL)) {
            gchar *icon = g_key_file_get_string(kf, "Desktop Entry", "Icon", NULL);
            if (icon && *icon) {
                gchar *id = g_strndup(name, strlen(name) - strlen(".desktop"));
                const char *resolved = NULL;
                gchar *lookup = NULL;
                if (g_path_is_absolute(icon) &&
                    g_str_has_prefix(icon, FLATPAK_EXPORT_ICONS "/") &&
                    g_file_test(icon, G_FILE_TEST_IS_REGULAR)) {
                    resolved = icon;
                } else {
                    gchar *base = g_path_get_basename(icon);
                    lookup = fp_icon_key_from_file(base);
                    g_free(base);
                    resolved = g_hash_table_lookup(index, lookup);
                }
                if (resolved) g_hash_table_replace(index, id, g_strdup(resolved));
                else g_free(id);
                g_free(lookup);
            }
            g_free(icon);
        }
        g_key_file_unref(kf);
        g_free(desktop_path);
    }
    g_dir_close(d);
}

static gint fp_string_sort(gconstpointer aa, gconstpointer bb) {
    return g_strcmp0((const char *)aa, (const char *)bb);
}

static gboolean fp_save_icon_index(GHashTable *index) {
    if (g_mkdir_with_parents(EAPT_DIR, 0755) != 0) return FALSE;
    GList *keys = g_hash_table_get_keys(index);
    keys = g_list_sort(keys, fp_string_sort);
    GString *out = g_string_new("# edpkg-gui-flatpak-icon-cache-v1\n");
    for (GList *l = keys; l; l = l->next) {
        const char *key = l->data;
        const char *path = g_hash_table_lookup(index, key);
        if (!key || !*key || !path || !*path) continue;
        gchar *k = fp_clean_field(key);
        gchar *v = fp_clean_field(path);
        g_string_append_printf(out, "%s\t%s\n", k, v);
        g_free(k); g_free(v);
    }
    g_list_free(keys);
    gboolean ok = g_file_set_contents(EAPT_ICON_INDEX, out->str, out->len, NULL);
    g_string_free(out, TRUE);
    return ok;
}

static GHashTable *fp_load_icon_index(void) {
    gchar *text = NULL;
    if (!g_file_get_contents(EAPT_ICON_INDEX, &text, NULL, NULL)) return NULL;
    gchar **lines = g_strsplit(text, "\n", -1);
    if (!lines[0] || strcmp(lines[0], "# edpkg-gui-flatpak-icon-cache-v1")) {
        g_strfreev(lines); g_free(text); return NULL;
    }
    GHashTable *index = fp_icon_index_new();
    for (int i = 1; lines[i]; i++) {
        if (!*lines[i]) continue;
        gchar **f = g_strsplit(lines[i], "\t", 2);
        if (g_strv_length(f) >= 2 && f[0] && *f[0] && f[1] && *f[1] &&
            g_str_has_prefix(f[1], FLATPAK_EXPORT_ICONS "/"))
            g_hash_table_replace(index, g_strdup(f[0]), g_strdup(f[1]));
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(text);
    return index;
}

static GHashTable *fp_rebuild_icon_index(GHashTable *apps, gboolean save) {
    (void)apps;
    GHashTable *index = fp_icon_index_new();
    fp_scan_icon_tree(index, FLATPAK_EXPORT_ICONS, 0);
    fp_add_desktop_icon_aliases(index);
    if (save) fp_save_icon_index(index);
    return index;
}

static GdkPixbuf *fp_load_pixbuf(const char *path, int size) {
    if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) return NULL;
    return gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, NULL);
}

static GdkPixbuf *fp_icon_for(FpPage *p, const char *app_id) {
    if (p->pixbuf_cache && app_id) {
        GdkPixbuf *cached = g_hash_table_lookup(p->pixbuf_cache, app_id);
        if (cached) return g_object_ref(cached);
    }
    const char *path = p->icon_index ? g_hash_table_lookup(p->icon_index, app_id) : NULL;
    GdkPixbuf *pb = fp_load_pixbuf(path, 28);
    if (!pb && p->fallback_icon) pb = g_object_ref(p->fallback_icon);
    if (pb && p->pixbuf_cache && app_id)
        g_hash_table_replace(p->pixbuf_cache, g_strdup(app_id), g_object_ref(pb));
    return pb;
}

static gint fp_app_ptr_cmp(gconstpointer aa, gconstpointer bb) {
    const FpApp *a = *(FpApp * const *)aa;
    const FpApp *b = *(FpApp * const *)bb;
    return g_utf8_collate(a->name ? a->name : a->id, b->name ? b->name : b->id);
}

static void fp_rebuild_sorted_apps(FpPage *p) {
    if (p->sorted_apps) g_ptr_array_free(p->sorted_apps, TRUE);
    p->sorted_apps = g_ptr_array_new();
    GHashTableIter it; gpointer key, value;
    g_hash_table_iter_init(&it, p->apps);
    while (g_hash_table_iter_next(&it, &key, &value)) g_ptr_array_add(p->sorted_apps, value);
    g_ptr_array_sort(p->sorted_apps, fp_app_ptr_cmp);
}

static const char *fp_action_text(FpAction action) {
    switch (action) {
        case FP_ACT_INSTALL: return ftr(FT_MARK_INSTALL);
        case FP_ACT_REMOVE: return ftr(FT_MARK_REMOVE);
        case FP_ACT_REINSTALL: return ftr(FT_MARK_REINSTALL);
        case FP_ACT_UPGRADE: return ftr(FT_UPGRADABLE);
        default: return "";
    }
}

static void fp_colors(FpApp *a, const char **bg, const char **fg, gboolean *set) {
    *set = FALSE; *bg = NULL; *fg = NULL;
    if (a->upgradable) { *bg = "#d97818"; *fg = "#ffffff"; *set = TRUE; }
}

static gboolean fp_app_matches_filter(FpPage *p, FpApp *a) {
    switch (p->filter_mode) {
        case FP_FILTER_INSTALLED: return a->installed;
        case FP_FILTER_UPGRADABLE: return a->upgradable;
        case FP_FILTER_NOT_INSTALLED: return !a->installed;
        default: return TRUE;
    }
}

static int fp_search_rank(FpPage *p, FpApp *a) {
    const char *q=p->search_folded ? p->search_folded : "";
    if(!*q || g_utf8_strlen(q,-1)<2 || !a->search_key) return 0;
    const char *name=a->search_key;
    const char *nl1=strchr(name,'\n');
    if(!nl1)return 0;
    const char *id=nl1+1;
    const char *nl2=strchr(id,'\n');
    if(!nl2)return 0;
    gsize name_len=(gsize)(nl1-name),id_len=(gsize)(nl2-id),qlen=strlen(q);
    if((name_len==qlen&&!memcmp(name,q,qlen))||(id_len==qlen&&!memcmp(id,q,qlen)))return 1;
    if((name_len>=qlen&&!memcmp(name,q,qlen))||(id_len>=qlen&&!memcmp(id,q,qlen)))return 2;
    if(g_strstr_len(name,(gssize)name_len,q)||g_strstr_len(id,(gssize)id_len,q))return 3;
    if(strstr(nl2+1,q))return 4;
    return 0;
}

static void fp_append_row(FpPage *p,FpApp *a){
    const char *bg=NULL,*fg=NULL;gboolean color=FALSE;
    fp_colors(a,&bg,&fg,&color);
    GdkPixbuf *icon=fp_icon_for(p,a->id);
    GtkTreeIter it;gtk_list_store_append(p->store,&it);
    gtk_list_store_set(p->store,&it,
        FP_C_ACTION,fp_action_text(a->action),FP_C_ICON,icon,FP_C_NAME,a->name,FP_C_ID,a->id,
        FP_C_INSTALLED,a->installed_version,FP_C_AVAILABLE,a->available_version,
        FP_C_BG,bg,FP_C_FG,fg,FP_C_HAS_COLOR,color,
        FP_C_STATE_ICON,a->upgradable ? p->icon_upgradable :
                        (a->installed ? p->icon_installed : NULL),-1);
    if(icon)g_object_unref(icon);
}

static void fp_rebuild_model(FpPage *p) {
    if (!p->sorted_apps) fp_rebuild_sorted_apps(p);
    gtk_tree_view_set_model(GTK_TREE_VIEW(p->tree), NULL);
    gtk_list_store_clear(p->store);
    const char *q=p->search_folded?p->search_folded:"";
    gboolean ranked=*q&&g_utf8_strlen(q,-1)>=2;
    if(!ranked){
        for(guint i=0;i<p->sorted_apps->len;i++){
            FpApp *a=g_ptr_array_index(p->sorted_apps,i);
            if(fp_app_matches_filter(p,a))fp_append_row(p,a);
        }
    }else{
        GPtrArray *bucket[4]={g_ptr_array_new(),g_ptr_array_new(),g_ptr_array_new(),g_ptr_array_new()};
        guint name_matches=0;
        for(guint i=0;i<p->sorted_apps->len;i++){
            FpApp *a=g_ptr_array_index(p->sorted_apps,i);
            /* Search results span the complete catalog regardless of sidebar filter. */
            int rank=fp_search_rank(p,a);
            if(rank>=1&&rank<=4){g_ptr_array_add(bucket[rank-1],a);if(rank<=3)name_matches++;}
        }
        for(int b=0;b<3;b++)for(guint i=0;i<bucket[b]->len;i++)fp_append_row(p,g_ptr_array_index(bucket[b],i));
        if(name_matches<50)for(guint i=0;i<bucket[3]->len;i++)fp_append_row(p,g_ptr_array_index(bucket[3],i));
        for(int b=0;b<4;b++)g_ptr_array_free(bucket[b],TRUE);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(p->tree), GTK_TREE_MODEL(p->store));
}

static GPtrArray *fp_selected_apps(FpPage *p) {
    GPtrArray *out = g_ptr_array_new();
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree));
    GtkTreeModel *model = NULL;
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
    for (GList *l = rows; l; l = l->next) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(model, &it, l->data)) {
            gchar *id = NULL;
            gtk_tree_model_get(model, &it, FP_C_ID, &id, -1);
            FpApp *a = id ? g_hash_table_lookup(p->apps, id) : NULL;
            if (a) g_ptr_array_add(out, a);
            g_free(id);
        }
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return out;
}

static void fp_refresh_details(FpPage *p) {
    GPtrArray *sel = fp_selected_apps(p);
    if (sel->len == 0) {
        gtk_label_set_text(GTK_LABEL(p->detail_name), ftr(FT_NO_SELECTION));
        gtk_label_set_text(GTK_LABEL(p->detail_desc), "");
        gtk_label_set_text(GTK_LABEL(p->detail_versions), "");
        gtk_widget_set_sensitive(p->btn_install, FALSE);
        gtk_widget_set_sensitive(p->btn_remove, FALSE);
        gtk_widget_set_sensitive(p->btn_reinstall, FALSE);
        gtk_widget_set_sensitive(p->btn_clear, FALSE);
    } else if (sel->len == 1) {
        FpApp *a = g_ptr_array_index(sel, 0);
        gtk_label_set_text(GTK_LABEL(p->detail_name), a->name);
        gtk_label_set_text(GTK_LABEL(p->detail_desc), a->description);
        gchar *v = g_strdup_printf("%s: %s    %s: %s    %s: %s",
                                    ftr(FT_APP_ID), a->id,
                                    ftr(FT_INSTALLED_VER), *a->installed_version ? a->installed_version : "—",
                                    ftr(FT_AVAILABLE_VER), *a->available_version ? a->available_version : "—");
        gtk_label_set_text(GTK_LABEL(p->detail_versions), v);
        g_free(v);
        gtk_widget_set_sensitive(p->btn_install, p->is_root && !a->installed);
        gtk_widget_set_sensitive(p->btn_remove, p->is_root && a->installed);
        gtk_widget_set_sensitive(p->btn_reinstall, p->is_root && a->installed);
        gtk_widget_set_sensitive(p->btn_clear, p->is_root && a->action != FP_ACT_NONE);
    } else {
        gchar *s = fp_format_count(FT_SELECTED_FMT, sel->len);
        gtk_label_set_text(GTK_LABEL(p->detail_name), s);
        g_free(s);
        gtk_label_set_text(GTK_LABEL(p->detail_desc), ftr(FT_MULTI_HELP));
        gtk_label_set_text(GTK_LABEL(p->detail_versions), "");
        gboolean can_install = FALSE, can_remove = FALSE, can_reinstall = FALSE, can_clear = FALSE;
        for (guint i = 0; i < sel->len; i++) {
            FpApp *a = g_ptr_array_index(sel, i);
            can_install |= !a->installed;
            can_remove |= a->installed;
            can_reinstall |= a->installed;
            can_clear |= a->action != FP_ACT_NONE;
        }
        gtk_widget_set_sensitive(p->btn_install, p->is_root && can_install);
        gtk_widget_set_sensitive(p->btn_remove, p->is_root && can_remove);
        gtk_widget_set_sensitive(p->btn_reinstall, p->is_root && can_reinstall);
        gtk_widget_set_sensitive(p->btn_clear, p->is_root && can_clear);
    }
    g_ptr_array_free(sel, TRUE);
}

static guint fp_marked_count(FpPage *p) {
    guint n = 0;
    GHashTableIter it; gpointer key, value;
    g_hash_table_iter_init(&it, p->apps);
    while (g_hash_table_iter_next(&it, &key, &value)) if (((FpApp *)value)->action != FP_ACT_NONE) n++;
    return n;
}

static void fp_update_status(FpPage *p) {
    guint marked = fp_marked_count(p);
    gchar *s = marked ? fp_format_count(FT_SELECTED_FMT, marked)
                      : fp_format_count(FT_COUNT_FMT, g_hash_table_size(p->apps));
    gtk_label_set_text(GTK_LABEL(p->status), s);
    g_free(s);
}

static void fp_set_selected_action(FpPage *p, FpAction action) {
    if (!p->is_root) return;
    GPtrArray *sel = fp_selected_apps(p);
    for (guint i = 0; i < sel->len; i++) {
        FpApp *a = g_ptr_array_index(sel, i);
        gboolean compatible = action == FP_ACT_NONE ||
                              (action == FP_ACT_INSTALL && !a->installed) ||
                              (action == FP_ACT_REMOVE && a->installed) ||
                              (action == FP_ACT_REINSTALL && a->installed);
        if (compatible) a->action = action;
    }
    g_ptr_array_free(sel, TRUE);
    fp_rebuild_model(p);
    fp_update_status(p);
    fp_refresh_details(p);
}

static void fp_reload_async(FpPage *p, gboolean force_remote);

static gboolean fp_search_idle(gpointer data) {
    FpPage *p = data;
    p->search_debounce_id = 0;
    gchar *q=g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(p->search)),-1);
    glong n=q?g_utf8_strlen(q,-1):0;
    glong prev=p->search_folded?g_utf8_strlen(p->search_folded,-1):0;
    if(n==1&&prev<=1){g_free(q);return G_SOURCE_REMOVE;}
    g_free(p->search_folded);p->search_folded=q;
    if(n>=2){
        /* Searching is global, independent of the sidebar filter. */
        p->filter_mode=FP_FILTER_ALL;
        if(!p->catalog_loaded&&!p->busy)fp_reload_async(p,FALSE);
    }
    fp_rebuild_model(p);
    fp_refresh_details(p);
    return G_SOURCE_REMOVE;
}

static void fp_on_search_changed(GtkEditable *e, gpointer data) {
    (void)e;
    FpPage *p = data;
    if (p->search_debounce_id) g_source_remove(p->search_debounce_id);
    p->search_debounce_id = g_timeout_add(120, fp_search_idle, p);
}

static void fp_on_selection_changed(GtkTreeSelection *sel, gpointer data) {
    (void)sel;
    fp_refresh_details(data);
}

static void fp_filter_clicked(GtkButton *b, gpointer data) {
    FpPage *p = data;
    p->filter_mode = (FpFilter)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "fp-filter"));
    if (p->filter_mode != FP_FILTER_INSTALLED && !p->catalog_loaded && !p->busy)
        fp_reload_async(p, FALSE);
    fp_rebuild_model(p);
    fp_refresh_details(p);
}

static GtkWidget *fp_sidebar_button(FpPage *p, const char *label, FpFilter filter) {
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_name(b, "side-button");
    gtk_widget_set_halign(b, GTK_ALIGN_FILL);
    g_object_set_data(G_OBJECT(b), "fp-filter", GINT_TO_POINTER(filter));
    g_signal_connect(b, "clicked", G_CALLBACK(fp_filter_clicked), p);
    return b;
}

static GtkWidget *fp_icon_button(const char *icon, const char *label) {
    GtkWidget *b = gtk_button_new();
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_add(GTK_CONTAINER(b), h);
    gtk_box_pack_start(GTK_BOX(h), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), gtk_label_new(label), FALSE, FALSE, 0);
    return b;
}

static void fp_add_text_column(FpPage *p, const char *title, int col, int width) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col,
        "cell-background", FP_C_BG, "foreground", FP_C_FG,
        "cell-background-set", FP_C_HAS_COLOR, "foreground-set", FP_C_HAS_COLOR, NULL);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->tree), c);
}

static void fp_add_icon_column(FpPage *p, int width) {
    GtkCellRenderer *r = gtk_cell_renderer_pixbuf_new();
    gtk_cell_renderer_set_fixed_size(r, 32, 32);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes("", r, "pixbuf", FP_C_ICON,
        "cell-background", FP_C_BG, "cell-background-set", FP_C_HAS_COLOR, NULL);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->tree), c);
}

/* Same installed (green) and upgrade (orange) indicators as APT/EDPKG.
 * Keep the application's own icon in its original column. These two pixbufs
 * are loaded only when the Flatpak tab is initialized, never per table row. */
static void fp_add_status_column(FpPage *p) {
    GtkCellRenderer *r = gtk_cell_renderer_pixbuf_new();
    g_object_set(r, "xalign", 0.5, NULL);
    gtk_cell_renderer_set_fixed_size(r, 28, 28);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes("", r,
        "pixbuf", FP_C_STATE_ICON,
        "cell-background", FP_C_BG, "cell-background-set", FP_C_HAS_COLOR, NULL);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, 34);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->tree), c);
}

static void fp_warn_root(FpPage *p) {
    GtkWidget *d = gtk_message_dialog_new(p->parent, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE,
                                          "%s", ftr(FT_ROOT_REQUIRED));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", ftr(FT_ROOT_REQUIRED_TEXT));
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

typedef struct {
    FpPage *page;
    gboolean force_remote;
    gboolean local_only;
} FpLoadJob;

typedef struct {
    FpPage *page;
    GHashTable *apps;
    GHashTable *icons;
    gchar *message;
    gboolean ok;
    gboolean catalog_loaded;
    gboolean force_remote;
} FpLoadResult;

static gboolean fp_progress_pulse(gpointer data);
static void fp_progress_dialog_destroyed(GtkWidget *w, gpointer data);
static void fp_refresh_progress_show(FpPage *p);

static gboolean fp_load_finish_idle(gpointer data) {
    FpLoadResult *r = data;
    FpPage *p = r->page;
    eapt_debug_log("FLATPAK load_finish ok=%d apps=%u icons=%u message=%s", r->ok,
        r->apps ? g_hash_table_size(r->apps) : 0, r->icons ? g_hash_table_size(r->icons) : 0,
        r->message ? r->message : "");
    p->busy = FALSE;
    p->catalog_loaded = r->catalog_loaded;
    gboolean refresh_ok = r->force_remote && r->ok && r->catalog_loaded && (!r->message || !*r->message);
    if(refresh_ok){
        p->repo_refreshed=TRUE;
        /* A completed Refresh always shows the complete new catalog. */
        p->filter_mode=FP_FILTER_ALL;
        g_free(p->search_folded);
        p->search_folded=g_strdup("");
        if(p->search && *gtk_entry_get_text(GTK_ENTRY(p->search)))
            gtk_entry_set_text(GTK_ENTRY(p->search),"");
    }
    if (p->sorted_apps) { g_ptr_array_free(p->sorted_apps, TRUE); p->sorted_apps = NULL; }
    if (p->apps) g_hash_table_destroy(p->apps);
    if (p->icon_index) g_hash_table_destroy(p->icon_index);
    p->apps = r->apps ? r->apps : fp_apps_new();
    p->icon_index = r->icons ? r->icons : fp_icon_index_new();
    if (p->pixbuf_cache) g_hash_table_remove_all(p->pixbuf_cache);
    fp_rebuild_sorted_apps(p);
    fp_rebuild_model(p);
    fp_update_status(p);
    if(p->repo_refreshed)gtk_widget_hide(p->repo_notice);
    else gtk_widget_show(p->repo_notice);
    if (r->message && *r->message) gtk_label_set_text(GTK_LABEL(p->status), r->message);
    gtk_widget_set_sensitive(p->btn_refresh, TRUE);
    gtk_widget_set_sensitive(p->btn_mark_upgrades, p->is_root && fp_program_exists());
    gtk_widget_set_sensitive(p->btn_apply, p->is_root && fp_program_exists());
    fp_refresh_details(p);
    if(r->force_remote && p->progress_dialog){
        if(p->pulse_id){ g_source_remove(p->pulse_id); p->pulse_id=0; }
        if(refresh_ok){
            GtkWidget *dlg=p->progress_dialog;
            gtk_window_set_modal(GTK_WINDOW(dlg),FALSE);
            gtk_widget_destroy(dlg);
            if(p->parent) gtk_window_present(p->parent);
        }else{
            const char *msg=(r->message&&*r->message)?r->message:ftr(FT_FAILED);
            if(p->progress_status) gtk_label_set_text(GTK_LABEL(p->progress_status),msg);
            if(p->progress_bar) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->progress_bar),0.0);
            if(p->progress_close) gtk_widget_set_sensitive(p->progress_close,TRUE);
            gtk_window_set_deletable(GTK_WINDOW(p->progress_dialog),TRUE);
        }
    }
    eapt_debug_widget("FLATPAK load_finish parent", GTK_WIDGET(p->parent));
    eapt_debug_schedule_toplevels("FLATPAK load_finish +250ms", 250);
    g_free(r->message);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer fp_load_worker(gpointer data) {
    FpLoadJob *j = data;
    FpLoadResult *r = g_new0(FpLoadResult, 1);
    r->page = j->page;
    r->force_remote = j->force_remote;

    if (!fp_program_exists()) {
        r->apps = fp_apps_new();
        r->icons = fp_load_icon_index();
        if (!r->icons) r->icons = fp_icon_index_new();
        r->message = g_strdup(ftr(FT_NOT_INSTALLED_TOOL));
        r->ok = FALSE;
        r->catalog_loaded = FALSE;
    } else if (j->local_only) {
        /* First paint: local installed state only. No remote, no remote-ls,
         * no AppStream update, and no icon-tree scan. */
        r->apps = fp_apps_new();
        fp_overlay_installed(r->apps);
        r->icons = fp_load_icon_index();
        if (!r->icons) r->icons = fp_icon_index_new();
        r->ok = TRUE;
        r->catalog_loaded = FALSE;
    } else {
        gchar *err = NULL;
        GHashTable *apps = fp_load_cache();
        GHashTable *icons = fp_load_icon_index();

        if (j->force_remote) {
            /* Refresh is the only path that may touch Flathub and rebuild both
             * persistent catalog and icon cache. */
            GHashTable *fresh = fp_build_remote_catalog(j->page->is_root, &err);
            if (fresh) {
                if (apps) g_hash_table_destroy(apps);
                apps = fresh;
                r->catalog_loaded = TRUE;
            } else {
                if (!apps) apps = fp_apps_new();
                fp_overlay_installed(apps);
                r->catalog_loaded = FALSE;
            }
            if (icons) g_hash_table_destroy(icons);
            icons = fp_rebuild_icon_index(apps, TRUE);
        } else {
            /* Normal catalog request: persistent cache only. */
            if (apps) {
                fp_overlay_installed(apps);
                r->catalog_loaded = TRUE;
            } else {
                apps = fp_apps_new();
                fp_overlay_installed(apps);
                r->catalog_loaded = FALSE;
                err = g_strdup(ftr(FT_OFFLINE_NO_CACHE));
            }
        }

        if (!icons) icons = fp_icon_index_new();
        r->apps = apps;
        r->icons = icons;
        r->message = err;
        r->ok = TRUE;
    }

    g_idle_add(fp_load_finish_idle, r);
    g_free(j);
    return NULL;
}

static void fp_start_load_async(FpPage *p, gboolean force_remote, gboolean local_only) {
    eapt_debug_log("FLATPAK load requested force_remote=%d local_only=%d busy=%d apps=%u",
        force_remote, local_only, p->busy, p->apps ? g_hash_table_size(p->apps) : 0);
    if (p->busy) { eapt_debug_log("FLATPAK load skipped: busy"); return; }
    p->busy = TRUE;
    gtk_label_set_text(GTK_LABEL(p->status), force_remote ? ftr(FT_REFRESHING) : ftr(FT_PREPARING));
    if(force_remote) fp_refresh_progress_show(p);
    gtk_widget_set_sensitive(p->btn_refresh, FALSE);
    gtk_widget_set_sensitive(p->btn_mark_upgrades, FALSE);
    gtk_widget_set_sensitive(p->btn_apply, FALSE);
    FpLoadJob *j = g_new0(FpLoadJob, 1);
    j->page = p;
    j->force_remote = force_remote;
    j->local_only = local_only;
    GThread *t = g_thread_new(local_only ? "edpkg-flatpak-local" : "edpkg-flatpak-catalog",
                              fp_load_worker, j);
    g_thread_unref(t);
}

static void fp_reload_async(FpPage *p, gboolean force_remote) {
    fp_start_load_async(p, force_remote, FALSE);
}

static void fp_load_local_async(FpPage *p) {
    fp_start_load_async(p, FALSE, TRUE);
}

static gboolean fp_progress_flush_cb(gpointer data);

static gboolean fp_progress_pulse(gpointer data) {
    FpPage *p = data;
    if (!p->busy || !p->progress_bar) return G_SOURCE_REMOVE;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(p->progress_bar));
    return G_SOURCE_CONTINUE;
}

static void fp_progress_dialog_destroyed(GtkWidget *w, gpointer data) {
    FpPage *p = data;
    eapt_debug_log("FLATPAK progress destroy callback dialog=%p current=%p busy=%d",
        (void*)w, (void*)p->progress_dialog, p->busy);
    eapt_debug_widget("FLATPAK progress destroy callback", w);
    if (p->progress_dialog == w) {
        p->progress_dialog = NULL;
        p->progress_close = NULL;
        p->progress_status = NULL;
        p->progress_bar = NULL;
        p->progress_text = NULL;
        p->progress_buffer = NULL;
        p->progress_scroller = NULL;
    }
    eapt_debug_schedule_toplevels("FLATPAK after progress destroy +100ms",100);
    eapt_debug_schedule_toplevels("FLATPAK after progress destroy +750ms",750);
}

static void fp_refresh_progress_show(FpPage *p) {
    if (p->pulse_id) { g_source_remove(p->pulse_id); p->pulse_id = 0; }
    if (p->progress_dialog) gtk_widget_destroy(p->progress_dialog);
    p->progress_dialog = gtk_dialog_new_with_buttons("Flatpak", p->parent, GTK_DIALOG_DESTROY_WITH_PARENT,
                                                     ftr(FT_DONE), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_modal(GTK_WINDOW(p->progress_dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(p->progress_dialog), 470, 135);
    gtk_window_set_deletable(GTK_WINDOW(p->progress_dialog), FALSE);
    g_signal_connect(p->progress_dialog, "destroy", G_CALLBACK(fp_progress_dialog_destroyed), p);
    p->progress_close = gtk_dialog_get_widget_for_response(GTK_DIALOG(p->progress_dialog), GTK_RESPONSE_CLOSE);
    gtk_widget_set_sensitive(p->progress_close, FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(p->progress_dialog));
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(v), 16);
    gtk_box_pack_start(GTK_BOX(box), v, TRUE, TRUE, 0);
    p->progress_status = gtk_label_new(ftr(FT_REFRESHING));
    gtk_label_set_xalign(GTK_LABEL(p->progress_status), 0.0f);
    gtk_box_pack_start(GTK_BOX(v), p->progress_status, FALSE, FALSE, 0);
    p->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(p->progress_bar), FALSE);
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(p->progress_bar), 0.06);
    gtk_box_pack_start(GTK_BOX(v), p->progress_bar, FALSE, FALSE, 0);
    g_signal_connect_swapped(p->progress_dialog, "response", G_CALLBACK(gtk_widget_destroy), p->progress_dialog);
    gtk_widget_show_all(p->progress_dialog);
    p->pulse_id = g_timeout_add(110, fp_progress_pulse, p);
}

static void fp_progress_show(FpPage *p) {
    if (p->progress_dialog) gtk_widget_destroy(p->progress_dialog);
    /* Keep the progress window transient, but not WM-modal.  An auto-closed
     * modal transient can leave Fluxbox with stale modal focus state once
     * PyMenu takes focus away from the EDPKG GUI parent. */
    p->progress_dialog = gtk_dialog_new_with_buttons("Flatpak", p->parent, GTK_DIALOG_DESTROY_WITH_PARENT,
                                                     ftr(FT_DONE), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_modal(GTK_WINDOW(p->progress_dialog), FALSE);
    eapt_debug_log("FLATPAK progress_show created dialog=%p parent=%p modal=%d",
        (void*)p->progress_dialog,(void*)p->parent,gtk_window_get_modal(GTK_WINDOW(p->progress_dialog)));
    eapt_debug_watch_window(p->progress_dialog,"FLATPAK-PROGRESS");
    g_signal_connect(p->progress_dialog, "destroy", G_CALLBACK(fp_progress_dialog_destroyed), p);
    gtk_window_set_default_size(GTK_WINDOW(p->progress_dialog), 680, 390);
    gtk_window_set_deletable(GTK_WINDOW(p->progress_dialog), FALSE);
    p->progress_close = gtk_dialog_get_widget_for_response(GTK_DIALOG(p->progress_dialog), GTK_RESPONSE_CLOSE);
    gtk_widget_set_sensitive(p->progress_close, FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(p->progress_dialog));
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(v), 12);
    gtk_box_pack_start(GTK_BOX(box), v, TRUE, TRUE, 0);

    p->progress_status = gtk_label_new(ftr(FT_RUNNING));
    gtk_label_set_xalign(GTK_LABEL(p->progress_status), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(p->progress_status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(v), p->progress_status, FALSE, FALSE, 0);

    p->progress_bar = gtk_progress_bar_new();
    /* The transfer output below already tells the user what Flatpak is doing.
     * Keep the bar in activity mode instead of showing a guessed percentage. */
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(p->progress_bar), FALSE);
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(p->progress_bar), 0.05);
    gtk_box_pack_start(GTK_BOX(v), p->progress_bar, FALSE, FALSE, 0);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    p->progress_scroller = sc;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sc, 660, 260);
    p->progress_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(p->progress_text), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(p->progress_text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(p->progress_text), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(p->progress_text), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(p->progress_text), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(p->progress_text), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(p->progress_text), 8);
    p->progress_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->progress_text));
    gtk_container_add(GTK_CONTAINER(sc), p->progress_text);
    gtk_box_pack_start(GTK_BOX(v), sc, TRUE, TRUE, 0);
    g_signal_connect_swapped(p->progress_dialog, "response", G_CALLBACK(gtk_widget_destroy), p->progress_dialog);
    gtk_widget_show_all(p->progress_dialog);
    eapt_debug_widget("FLATPAK progress shown",p->progress_dialog);
    eapt_debug_toplevels("FLATPAK progress shown");
    p->pulse_id = g_timeout_add(120, fp_progress_pulse, p);
    if (p->progress_flush_id) g_source_remove(p->progress_flush_id);
    p->progress_flush_id = g_timeout_add(100, fp_progress_flush_cb, p);
}

static gboolean fp_progress_at_bottom(FpPage *p) {
    if (!p->progress_scroller) return TRUE;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->progress_scroller));
    if (!adj) return TRUE;
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >= gtk_adjustment_get_upper(adj) - 8.0;
}

static void fp_progress_trim(FpPage *p) {
    if (!p->progress_buffer) return;
    gint chars = gtk_text_buffer_get_char_count(p->progress_buffer);
    if (chars <= 500000) return;
    GtkTextIter start, cut;
    gtk_text_buffer_get_start_iter(p->progress_buffer, &start);
    gtk_text_buffer_get_iter_at_offset(p->progress_buffer, &cut, chars - 400000);
    gtk_text_buffer_delete(p->progress_buffer, &start, &cut);
}

static void fp_progress_append(FpPage *p, const char *text) {
    if (!p->progress_buffer || !text || !*text) return;
    gboolean at_bottom = fp_progress_at_bottom(p);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(p->progress_buffer, &end);
    gtk_text_buffer_insert(p->progress_buffer, &end, text, -1);
    if (text[strlen(text)-1] != '\n') gtk_text_buffer_insert(p->progress_buffer, &end, "\n", 1);
    fp_progress_trim(p);
    if (at_bottom && p->progress_text) {
        gtk_text_buffer_get_end_iter(p->progress_buffer, &end);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(p->progress_text), &end, 0.0, FALSE, 0.0, 1.0);
    }
}

static gboolean fp_progress_flush_cb(gpointer data) {
    FpPage *p = data;
    gchar *chunk = NULL, *status = NULL;
    g_mutex_lock(&p->progress_lock);
    if (p->progress_pending && p->progress_pending->len) {
        chunk = g_strdup(p->progress_pending->str);
        g_string_truncate(p->progress_pending, 0);
    }
    if (p->progress_pending_status) {
        status = p->progress_pending_status;
        p->progress_pending_status = NULL;
    }
    g_mutex_unlock(&p->progress_lock);
    if (chunk) { fp_progress_append(p, chunk); g_free(chunk); }
    if (status && p->progress_status) gtk_label_set_text(GTK_LABEL(p->progress_status), status);
    g_free(status);
    if(!p->busy){p->progress_flush_id=0;return G_SOURCE_REMOVE;}
    return G_SOURCE_CONTINUE;
}

static void fp_progress_append_async(FpPage *p, const char *text) {
    if (!text || !*text) return;
    g_mutex_lock(&p->progress_lock);
    if (!p->progress_pending) p->progress_pending = g_string_new("");
    g_string_append(p->progress_pending, text);
    if (text[strlen(text)-1] != '\n') g_string_append_c(p->progress_pending, '\n');
    g_mutex_unlock(&p->progress_lock);
}

static void fp_progress_update_async(FpPage *p, gdouble fraction, const char *status) {
    (void)fraction;
    if (!status || !*status) return;
    g_mutex_lock(&p->progress_lock);
    g_free(p->progress_pending_status);
    p->progress_pending_status = g_strdup(status);
    g_mutex_unlock(&p->progress_lock);
}

/* Parse the progress Flatpak prints while stdout is piped.  Lines such as
 * "Installing 2/5 ... 34%" contain both the current ref and its percentage.
 * Combining both values gives a much more useful progress bar than merely
 * pulsing while the subprocess runs. */
static gdouble fp_flatpak_line_progress(const char *line, gdouble previous) {
    if (!line || !*line) return previous;

    gint current = -1, total = -1, percent = -1;
    /* One transaction runs at a time, so these compiled expressions can be
     * reused for every streamed line without allocation churn. */
    static GRegex *pair = NULL;
    static GRegex *pct = NULL;
    if (!pair) pair = g_regex_new("([0-9]+)/([0-9]+)", 0, 0, NULL);
    if (!pct) pct = g_regex_new("([0-9]{1,3})%", 0, 0, NULL);
    GMatchInfo *mi = NULL;

    if (g_regex_match(pair, line, 0, &mi)) {
        gchar *a = g_match_info_fetch(mi, 1);
        gchar *b = g_match_info_fetch(mi, 2);
        current = a ? atoi(a) : -1;
        total = b ? atoi(b) : -1;
        g_free(a); g_free(b);
    }
    if (mi) g_match_info_free(mi);
    mi = NULL;

    if (g_regex_match(pct, line, 0, &mi)) {
        gchar *v = g_match_info_fetch(mi, 1);
        percent = v ? atoi(v) : -1;
        g_free(v);
    }
    if (mi) g_match_info_free(mi);

    gdouble local = previous;
    if (current > 0 && total > 0) {
        if (percent >= 0)
            local = ((gdouble)(current - 1) + CLAMP(percent, 0, 100) / 100.0) / (gdouble)total;
        else
            local = (gdouble)(current - 1) / (gdouble)total;
    } else if (percent >= 0) {
        local = CLAMP(percent, 0, 100) / 100.0;
    }

    /* Do not let noisy/restarted sub-progress make the bar move backwards. */
    if (local < previous) local = previous;
    return CLAMP(local, 0.0, 0.995);
}

static gboolean fp_line_is_dynamic_progress(const char *line) {
    if (!line || !*line) return FALSE;
    /* Flatpak redraws transfer rows with CR.  When output is not attached to
     * a real terminal those redraws must feed the GUI progress widgets, not
     * become hundreds of rows in the console. */
    static GRegex *pair = NULL, *pct = NULL;
    if (!pair) pair = g_regex_new("[0-9]+/[0-9]+", 0, 0, NULL);
    if (!pct) pct = g_regex_new("[0-9]{1,3}%", 0, 0, NULL);
    if (g_regex_match(pair, line, 0, NULL) && g_regex_match(pct, line, 0, NULL)) return TRUE;
    if (g_regex_match(pct, line, 0, NULL) &&
        (strstr(line, "B/s") || strstr(line, "Installing") || strstr(line, "Downloading") ||
         strstr(line, "Receiving") || strstr(line, "Deploying") || strstr(line, "Updating"))) return TRUE;
    return FALSE;
}

static gchar *fp_clean_terminal_text(const char *src) {
    if (!src) return g_strdup("");
    /* Remove ANSI CSI sequences.  The progress bar is represented by the GTK
     * widget, so terminal colour/cursor control bytes have no place in the
     * text view. */
    static GRegex *ansi = NULL;
    if (!ansi) ansi = g_regex_new("\\x1B\\[[0-?]*[ -/]*[@-~]", 0, 0, NULL);
    GError *error = NULL;
    gchar *clean = g_regex_replace(ansi, src, -1, 0, "", 0, &error);
    if (error) { g_error_free(error); return g_strdup(src); }
    return clean ? clean : g_strdup(src);
}

static void fp_process_stream_segment(FpPage *p, const char *segment,
                                      guint item_index, guint item_count,
                                      gdouble *local_progress,
                                      gboolean carriage_only) {
    if (!segment) return;
    gchar *clean = fp_clean_terminal_text(segment);
    gchar *line = g_strstrip(clean);
    if (!*line) { g_free(clean); return; }

    gdouble previous = *local_progress;
    gdouble next = fp_flatpak_line_progress(line, previous);
    gboolean dynamic = carriage_only || fp_line_is_dynamic_progress(line);

    if (next > previous) {
        *local_progress = next;
        gdouble overall = ((gdouble)item_index + next) / (gdouble)MAX(item_count, 1u);
        fp_progress_update_async(p, overall, line);
    } else if (dynamic) {
        /* Keep the current transfer text visible in the status label even if
         * the parsed percentage did not advance in this repaint. */
        gdouble overall = ((gdouble)item_index + previous) / (gdouble)MAX(item_count, 1u);
        fp_progress_update_async(p, overall, line);
    }

    /* A CR is a terminal repaint, not a new log line.  Percentage/speed rows
     * are also transient even if Flatpak terminates one with NL. */
    if (!dynamic) fp_progress_append_async(p, line);
    g_free(clean);
}

static gint fp_run_flatpak_stream(FpPage *p, gchar *argv[], guint item_index, guint item_count) {
    g_mkdir_with_parents(FLATPAK_TMPDIR, 0700);

    GError *error = NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_setenv(launcher, "TMPDIR", FLATPAK_TMPDIR, TRUE);

    GSubprocess *proc = g_subprocess_launcher_spawnv(
        launcher, (const gchar * const *)argv, &error);
    g_object_unref(launcher);
    if (!proc) {
        fp_progress_append_async(p, error ? error->message : "Flatpak spawn failed");
        if (error) g_error_free(error);
        return -1;
    }

    GInputStream *stream = g_subprocess_get_stdout_pipe(proc);
    GString *pending = g_string_new("");
    gdouble local_progress = 0.0;
    guint8 buf[4096];
    gssize nr;

    while ((nr = g_input_stream_read(stream, buf, sizeof(buf), NULL, &error)) > 0) {
        g_string_append_len(pending, (const gchar *)buf, nr);
        for (;;) {
            gchar *cr = strchr(pending->str, '\r');
            gchar *nl = strchr(pending->str, '\n');
            gchar *cut = NULL;
            if (cr && nl) cut = cr < nl ? cr : nl;
            else cut = cr ? cr : nl;
            if (!cut) break;

            gsize len = (gsize)(cut - pending->str);
            gboolean carriage_only = (*cut == '\r');
            gsize consume = 1;
            /* CRLF is one completed line; a lone CR is a repaint. */
            if (carriage_only && len + 1 < pending->len && pending->str[len + 1] == '\n') {
                carriage_only = FALSE;
                consume = 2;
            }

            gchar *line = g_strndup(pending->str, len);
            fp_process_stream_segment(p, line, item_index, item_count, &local_progress, carriage_only);
            g_free(line);
            g_string_erase(pending, 0, len + consume);
        }
    }

    if (pending->len)
        fp_process_stream_segment(p, pending->str, item_index, item_count, &local_progress, FALSE);
    g_string_free(pending, TRUE);

    if (error) {
        fp_progress_append_async(p, error->message);
        g_error_free(error);
        error = NULL;
    }

    if (!g_subprocess_wait(proc, NULL, &error)) {
        fp_progress_append_async(p, error ? error->message : "Flatpak wait failed");
        if (error) g_error_free(error);
        g_object_unref(proc);
        return -1;
    }

    gint rc = -1;
    if (g_subprocess_get_if_exited(proc))
        rc = g_subprocess_get_exit_status(proc);
    else if (g_subprocess_get_if_signaled(proc))
        rc = 128 + g_subprocess_get_term_sig(proc);

    if (rc == 0) {
        gdouble done = (gdouble)(item_index + 1) / (gdouble)MAX(item_count, 1u);
        fp_progress_update_async(p, done, ftr(FT_RUNNING));
    }
    g_object_unref(proc);
    return rc;
}

typedef struct { char *id; char *remote; FpAction action; } FpTxnItem;
static void fp_txn_item_free(gpointer ptr) {
    FpTxnItem *i = ptr;
    if (!i) return;
    g_free(i->id); g_free(i->remote); g_free(i);
}

/* Refresh the desktop/menu database after a successful Flatpak transaction.
 * Essora now prefers the public `fixmenus` command; keep the older Store
 * helper name as a compatibility fallback for installations that still ship
 * /usr/local/bin/fixmenu.  EDPKG GUI already runs as root through its gksu
 * launcher, so no privilege wrapper is needed here. */
static void fp_run_fixmenus(void) {
    const char *candidates[] = {
        FIXMENUS_BIN,
        "/usr/local/bin/fixmenus",
        FIXMENU_COMPAT_BIN,
        "/usr/bin/fixmenu",
        NULL
    };

    for (guint i = 0; candidates[i]; i++) {
        if (!g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE)) continue;
        char *argv[] = { (char *)candidates[i], NULL };
        gint rc = 0;
        gchar *out = fp_capture(argv, &rc);
        g_free(out);
        return;
    }

    /* Also honour PATH in case a distribution installs the helper elsewhere. */
    gchar *path = g_find_program_in_path("fixmenus");
    if (!path) path = g_find_program_in_path("fixmenu");
    if (path) {
        char *argv[] = { path, NULL };
        gint rc = 0;
        gchar *out = fp_capture(argv, &rc);
        g_free(out);
        g_free(path);
    }
}

typedef struct { FpPage *page; GPtrArray *items; } FpTxnJob;

typedef struct {
    FpPage *page;
    GHashTable *apps;
    GHashTable *icons;
    gboolean ok;
    gchar *message;
} FpTxnResult;

static gboolean fp_txn_finish_idle(gpointer data) {
    FpTxnResult *r = data;
    FpPage *p = r->page;
    eapt_debug_log("FLATPAK txn_finish begin ok=%d progress=%p parent=%p message=%s",
        r->ok,(void*)p->progress_dialog,(void*)p->parent,r->message?r->message:"");
    eapt_debug_toplevels("FLATPAK txn_finish begin");
    p->busy = FALSE;
    if (p->pulse_id) { g_source_remove(p->pulse_id); p->pulse_id = 0; }
    if (p->progress_flush_id) { g_source_remove(p->progress_flush_id); p->progress_flush_id = 0; }
    (void)fp_progress_flush_cb(p);
    if (p->progress_bar) {
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(p->progress_bar), FALSE);
        if(!r->ok)gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->progress_bar),0.0);
    }
    if (p->progress_status)
        gtk_label_set_text(GTK_LABEL(p->progress_status), r->ok ? ftr(FT_DONE) : ftr(FT_FAILED));
    if (r->message && *r->message) fp_progress_append(p, r->message);
    if (p->progress_close) gtk_widget_set_sensitive(p->progress_close, TRUE);
    if (p->progress_dialog) gtk_window_set_deletable(GTK_WINDOW(p->progress_dialog), TRUE);
    /* Successful package transactions return immediately to the main GUI.
     * On failure keep the dialog open so the user can read the error. */
    if (r->ok && p->progress_dialog) {
        GtkWidget *dlg = p->progress_dialog;
        eapt_debug_widget("FLATPAK txn_finish before progress destroy",dlg);
        gtk_window_set_modal(GTK_WINDOW(dlg), FALSE);
        gtk_widget_destroy(dlg);
        eapt_debug_log("FLATPAK txn_finish after progress destroy current=%p",(void*)p->progress_dialog);
        if (p->parent) {
            eapt_debug_widget("FLATPAK txn_finish before parent present",GTK_WIDGET(p->parent));
            gtk_window_present(p->parent);
            eapt_debug_widget("FLATPAK txn_finish after parent present",GTK_WIDGET(p->parent));
        }
        eapt_debug_toplevels("FLATPAK txn_finish after parent present");
        eapt_debug_schedule_toplevels("FLATPAK return-main +250ms",250);
        eapt_debug_schedule_toplevels("FLATPAK return-main +1000ms",1000);
    }
    if (p->sorted_apps) { g_ptr_array_free(p->sorted_apps, TRUE); p->sorted_apps = NULL; }
    if (p->apps) g_hash_table_destroy(p->apps);
    if (p->icon_index) g_hash_table_destroy(p->icon_index);
    p->apps = r->apps ? r->apps : fp_apps_new();
    p->icon_index = r->icons ? r->icons : fp_icon_index_new();
    if (p->pixbuf_cache) g_hash_table_remove_all(p->pixbuf_cache);
    fp_rebuild_sorted_apps(p);
    fp_rebuild_model(p);
    fp_update_status(p);
    gtk_widget_set_sensitive(p->btn_refresh, TRUE);
    gtk_widget_set_sensitive(p->btn_mark_upgrades, p->is_root);
    gtk_widget_set_sensitive(p->btn_apply, p->is_root);
    fp_refresh_details(p);
    g_free(r->message);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer fp_txn_worker(gpointer data) {
    FpTxnJob *j = data;
    gboolean all_ok = TRUE;
    gboolean menu_changed = FALSE;
    GString *errors = g_string_new("");
    guint total_items = MAX(j->items->len, 1u);
    for (guint n = 0; n < j->items->len; n++) {
        FpTxnItem *it = g_ptr_array_index(j->items, n);
        gchar *head = g_strdup_printf("%s: %s", fp_action_text(it->action), it->id);
        fp_progress_append_async(j->page, head);
        gchar *status = g_strdup_printf("%s %s", ftr(FT_RUNNING), it->id);
        fp_progress_update_async(j->page, (gdouble)n / (gdouble)total_items, status);
        g_free(status);
        g_free(head);

        char *argv_install[] = { FLATPAK_BIN, "--system", "install", "-y", it->remote, it->id, NULL };
        char *argv_reinstall[] = { FLATPAK_BIN, "--system", "install", "--reinstall", "-y", it->remote, it->id, NULL };
        char *argv_remove[] = { FLATPAK_BIN, "--system", "uninstall", "-y", it->id, NULL };
        char *argv_upgrade[] = { FLATPAK_BIN, "--system", "update", "-y", it->id, NULL };
        char **argv = NULL;
        if (it->action == FP_ACT_INSTALL) argv = argv_install;
        else if (it->action == FP_ACT_REINSTALL) argv = argv_reinstall;
        else if (it->action == FP_ACT_REMOVE) argv = argv_remove;
        else if (it->action == FP_ACT_UPGRADE) argv = argv_upgrade;
        if (!argv) continue;

        gint rc = fp_run_flatpak_stream(j->page, argv, n, total_items);
        if (rc != 0) {
            all_ok = FALSE;
            g_string_append_printf(errors, "%s (code %d)\n", it->id, rc);
        } else {
            /* Install/reinstall/remove/update can create, replace or remove
             * exported .desktop files, so rebuild menus once after the whole
             * successful batch instead of once per application. */
            menu_changed = TRUE;
        }
    }

    if (menu_changed) fp_run_fixmenus();

    /* Transactions only refresh local installed state.  They deliberately do
     * not rescan the icon tree or query the whole Flathub catalog.  The user
     * controlled Refresh button performs that full cache regeneration. */
    GHashTable *apps = fp_load_cache();
    if (!apps) apps = fp_apps_new();
    fp_overlay_installed(apps);
    GHashTableIter hit; gpointer key, value;
    g_hash_table_iter_init(&hit, apps);
    while (g_hash_table_iter_next(&hit, &key, &value)) {
        FpApp *a = value;
        if (a->installed && a->installed_version && *a->installed_version &&
            a->available_version && *a->available_version &&
            !g_strcmp0(a->installed_version, a->available_version))
            a->upgradable = FALSE;
        a->action = FP_ACT_NONE;
    }
    fp_save_cache(apps);

    GHashTable *icons = fp_load_icon_index();
    if (!icons) icons = fp_icon_index_new();
    FpTxnResult *r = g_new0(FpTxnResult, 1);
    r->page = j->page;
    r->apps = apps;
    r->icons = icons;
    r->ok = all_ok;
    if (errors->len) r->message = g_string_free(errors, FALSE);
    else { g_string_free(errors, TRUE); r->message = g_strdup(""); }
    g_idle_add(fp_txn_finish_idle, r);
    g_ptr_array_free(j->items, TRUE);
    g_free(j);
    return NULL;
}

static void fp_on_refresh(GtkButton *b, gpointer data) {
    (void)b;
    FpPage *p = data;
    eapt_debug_log("FLATPAK refresh clicked busy=%d",p->busy);
    eapt_debug_toplevels("FLATPAK refresh clicked");
    if (!p->is_root) { fp_warn_root(p); return; }
    fp_reload_async(p, TRUE);
}

static void fp_on_mark_upgrades(GtkButton *b, gpointer data) {
    (void)b;
    FpPage *p = data;
    if (!p->is_root) { fp_warn_root(p); return; }
    GHashTableIter it; gpointer key, value;
    g_hash_table_iter_init(&it, p->apps);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        FpApp *a = value;
        if (a->upgradable) a->action = FP_ACT_UPGRADE;
    }
    fp_rebuild_model(p); fp_update_status(p); fp_refresh_details(p);
}

static void fp_on_apply(GtkButton *b, gpointer data) {
    (void)b;
    FpPage *p = data;
    eapt_debug_log("FLATPAK apply clicked busy=%d marked=%u",p->busy,fp_marked_count(p));
    eapt_debug_toplevels("FLATPAK apply clicked");
    if (!p->is_root) { fp_warn_root(p); return; }
    if (!fp_marked_count(p)) {
        GtkWidget *d = gtk_message_dialog_new(p->parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                                              "%s", ftr(FT_NO_MARKS));
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d); return;
    }
    GtkWidget *q = gtk_message_dialog_new(p->parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
                                          "%s", ftr(FT_CONFIRM_APPLY));
    int resp = gtk_dialog_run(GTK_DIALOG(q)); gtk_widget_destroy(q);
    if (resp != GTK_RESPONSE_OK) return;

    FpTxnJob *j = g_new0(FpTxnJob, 1);
    j->page = p;
    j->items = g_ptr_array_new_with_free_func(fp_txn_item_free);
    GHashTableIter it; gpointer key, value;
    g_hash_table_iter_init(&it, p->apps);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        FpApp *a = value;
        if (a->action == FP_ACT_NONE) continue;
        FpTxnItem *x = g_new0(FpTxnItem, 1);
        x->id = g_strdup(a->id);
        x->remote = g_strdup((a->remote && *a->remote) ? a->remote : FLATPAK_REMOTE);
        x->action = a->action;
        g_ptr_array_add(j->items, x);
    }
    eapt_debug_log("FLATPAK transaction starting items=%u",j->items->len);
    p->busy = TRUE;
    gtk_widget_set_sensitive(p->btn_refresh, FALSE);
    gtk_widget_set_sensitive(p->btn_mark_upgrades, FALSE);
    gtk_widget_set_sensitive(p->btn_apply, FALSE);
    fp_progress_show(p);
    GThread *t = g_thread_new("edpkg-flatpak-transaction", fp_txn_worker, j);
    g_thread_unref(t);
}

static void fp_on_install(GtkButton *b, gpointer d) { (void)b; fp_set_selected_action(d, FP_ACT_INSTALL); }
static void fp_on_remove(GtkButton *b, gpointer d) { (void)b; fp_set_selected_action(d, FP_ACT_REMOVE); }
static void fp_on_reinstall(GtkButton *b, gpointer d) { (void)b; fp_set_selected_action(d, FP_ACT_REINSTALL); }
static void fp_on_clear(GtkButton *b, gpointer d) { (void)b; fp_set_selected_action(d, FP_ACT_NONE); }

GtkWidget *flatpak_page_new(GtkWindow *parent, gboolean is_root) {
    eapt_debug_log("FLATPAK page_new parent=%p is_root=%d",(void*)parent,is_root);
    fp_choose_language();
    FpPage *p = g_new0(FpPage, 1);
    g_mutex_init(&p->progress_lock);
    p->pixbuf_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    p->parent = parent;
    p->is_root = is_root;
    p->apps = fp_apps_new();
    p->icon_index = fp_icon_index_new();
    p->search_folded = g_strdup("");
    p->filter_mode = FP_FILTER_INSTALLED;
    p->fallback_icon = fp_load_pixbuf(FLATPAK_FALLBACK_ICON, 28);
    p->icon_installed = fp_load_pixbuf(FP_INSTALLED_ICON, 24);
    p->icon_upgradable = fp_load_pixbuf(FP_UPGRADABLE_ICON, 24);

    p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_name(tb, "toolbar");
    p->btn_refresh = fp_icon_button("view-refresh", ftr(FT_REFRESH));
    p->btn_mark_upgrades = fp_icon_button("software-update-available", ftr(FT_MARK_UPGRADES));
    p->btn_apply = fp_icon_button("emblem-default", ftr(FT_APPLY));
    gtk_widget_set_name(p->btn_apply, "apply");
    gtk_box_pack_start(GTK_BOX(tb), p->btn_refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), p->btn_mark_upgrades, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), p->btn_apply, FALSE, FALSE, 0);
    p->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->search), ftr(FT_SEARCH));
    gtk_widget_set_size_request(p->search, 260, -1);
    gtk_box_pack_end(GTK_BOX(tb), p->search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->root), tb, FALSE, FALSE, 0);

    g_signal_connect(p->btn_refresh, "clicked", G_CALLBACK(fp_on_refresh), p);
    g_signal_connect(p->btn_mark_upgrades, "clicked", G_CALLBACK(fp_on_mark_upgrades), p);
    g_signal_connect(p->btn_apply, "clicked", G_CALLBACK(fp_on_apply), p);
    g_signal_connect(p->search, "changed", G_CALLBACK(fp_on_search_changed), p);

    p->repo_notice = gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(p->repo_notice),GTK_MESSAGE_INFO);
    GtkWidget *repo_msg = gtk_label_new(ftr(FT_REPO_NOTICE));
    gtk_label_set_xalign(GTK_LABEL(repo_msg),0);
    gtk_label_set_line_wrap(GTK_LABEL(repo_msg),TRUE);
    gtk_widget_set_margin_start(repo_msg,8);
    gtk_widget_set_margin_end(repo_msg,8);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(p->repo_notice))),repo_msg);
    gtk_box_pack_start(GTK_BOX(p->root),p->repo_notice,FALSE,FALSE,0);
    GtkWidget *pan = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(p->root), pan, TRUE, TRUE, 0);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_name(side, "side");
    gtk_widget_set_size_request(side, 180, -1);
    p->filter_buttons[FP_FILTER_ALL] = fp_sidebar_button(p, ftr(FT_ALL), FP_FILTER_ALL);
    p->filter_buttons[FP_FILTER_INSTALLED] = fp_sidebar_button(p, ftr(FT_INSTALLED), FP_FILTER_INSTALLED);
    p->filter_buttons[FP_FILTER_UPGRADABLE] = fp_sidebar_button(p, ftr(FT_UPGRADABLE), FP_FILTER_UPGRADABLE);
    p->filter_buttons[FP_FILTER_NOT_INSTALLED] = fp_sidebar_button(p, ftr(FT_NOT_INSTALLED), FP_FILTER_NOT_INSTALLED);
    for (int i = 0; i < 4; i++) gtk_box_pack_start(GTK_BOX(side), p->filter_buttons[i], FALSE, FALSE, 0);
    gtk_paned_pack1(GTK_PANED(pan), side, FALSE, FALSE);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_pack2(GTK_PANED(pan), right, TRUE, FALSE);
    p->store = gtk_list_store_new(FP_N_COLS, G_TYPE_STRING, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF);
    p->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->tree), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(p->tree), FALSE);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(p->tree), TRUE);
    fp_add_text_column(p, ftr(FT_ACTION), FP_C_ACTION, 90);
    fp_add_status_column(p);
    fp_add_icon_column(p, 42);
    fp_add_text_column(p, ftr(FT_APP), FP_C_NAME, 240);
    fp_add_text_column(p, ftr(FT_APP_ID), FP_C_ID, 260);
    fp_add_text_column(p, ftr(FT_INSTALLED_VER), FP_C_INSTALLED, 125);
    fp_add_text_column(p, ftr(FT_AVAILABLE_VER), FP_C_AVAILABLE, 125);
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sc), p->tree);
    gtk_box_pack_start(GTK_BOX(right), sc, TRUE, TRUE, 0);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(fp_on_selection_changed), p);

    GtkWidget *det = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_name(det, "details");
    p->detail_name = gtk_label_new(ftr(FT_NO_SELECTION));
    gtk_widget_set_name(p->detail_name, "details-title");
    gtk_label_set_xalign(GTK_LABEL(p->detail_name), 0);
    p->detail_desc = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->detail_desc), 0);
    gtk_label_set_line_wrap(GTK_LABEL(p->detail_desc), TRUE);
    p->detail_versions = gtk_label_new("");
    gtk_widget_set_name(p->detail_versions, "dim");
    gtk_label_set_xalign(GTK_LABEL(p->detail_versions), 0);
    gtk_box_pack_start(GTK_BOX(det), p->detail_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(det), p->detail_desc, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(det), p->detail_versions, FALSE, FALSE, 0);
    GtkWidget *db = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->btn_install = gtk_button_new_with_label(ftr(FT_MARK_INSTALL));
    p->btn_remove = gtk_button_new_with_label(ftr(FT_MARK_REMOVE));
    p->btn_reinstall = gtk_button_new_with_label(ftr(FT_MARK_REINSTALL));
    p->btn_clear = gtk_button_new_with_label(ftr(FT_CLEAR_MARK));
    gtk_box_pack_start(GTK_BOX(db), p->btn_install, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(db), p->btn_remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(db), p->btn_reinstall, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(db), p->btn_clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(det), db, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), det, FALSE, FALSE, 0);
    g_signal_connect(p->btn_install, "clicked", G_CALLBACK(fp_on_install), p);
    g_signal_connect(p->btn_remove, "clicked", G_CALLBACK(fp_on_remove), p);
    g_signal_connect(p->btn_reinstall, "clicked", G_CALLBACK(fp_on_reinstall), p);
    g_signal_connect(p->btn_clear, "clicked", G_CALLBACK(fp_on_clear), p);

    p->status = gtk_label_new(ftr(FT_READY));
    gtk_widget_set_name(p->status, "statusbar");
    gtk_label_set_xalign(GTK_LABEL(p->status), 0);
    gtk_box_pack_start(GTK_BOX(p->root), p->status, FALSE, FALSE, 0);

    gboolean have_flatpak = fp_program_exists();
    gtk_widget_set_sensitive(p->btn_refresh, TRUE);
    gtk_widget_set_sensitive(p->btn_mark_upgrades, p->is_root && have_flatpak);
    gtk_widget_set_sensitive(p->btn_apply, p->is_root && have_flatpak);
    fp_refresh_details(p);
    fp_load_local_async(p);
    return p->root;
}
