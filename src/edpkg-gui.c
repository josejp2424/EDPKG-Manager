#define _GNU_SOURCE
#include <gtk/gtk.h>
#include "flatpak-page.h"
#include "appimage-page.h"
#include "debug-log.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID "org.essora.edpkg.gui"
#define APP_VERSION "0.1.40"
#define EDPKG_BIN "/usr/bin/edpkg"
#define APT_BIN "/usr/bin/apt"
#define APTGET_BIN "/usr/bin/apt-get"
#define APTCACHE_BIN "/usr/bin/apt-cache"
#define DPKG_BIN "/usr/bin/dpkg"
#define GUI_LIBDIR "/usr/lib/edpkg-gui"
#define ICON_FILE GUI_LIBDIR "/edpkg-manager.png"
#define ICON_SVG_FILE GUI_LIBDIR "/edpkg-manager.svg"
#define ICON_INSTALLED_FILE GUI_LIBDIR "/package-installed.png"
#define ICON_BROKEN_FILE GUI_LIBDIR "/package-broken.png"
#define ICON_UPGRADABLE_FILE GUI_LIBDIR "/package-upgradable.png"
#define CACHE_DIR "/var/cache/edpkg-gui"
#define EDPKG_STATUS_FILE "/var/lib/edpkg/status"
#define EDPKG_LISTS_DIR "/var/lib/edpkg/lists"
#define DPKG_STATUS_FILE "/var/lib/dpkg/status"
#define APT_LISTS_DIR "/var/lib/apt/lists"

/* The EDPKG binary override exists only for the explicit guard selftest.
 * Normal GUI execution, including root/gksu sessions, always uses /usr/bin/edpkg. */
static gboolean edpkg_test_override_allowed = FALSE;

static const char *edpkg_bin_path(void) {
    if(edpkg_test_override_allowed){
        const char *override = g_getenv("EDPKG_GUI_EDPKG_BIN");
        if(override && *override)return override;
    }
    return EDPKG_BIN;
}

typedef enum {
    BACKEND_NONE,
    BACKEND_EDPKG,
    BACKEND_APT
} BackendMode;

typedef enum {
    F_ALL,
    F_INSTALLED,
    F_UPGRADABLE,
    F_NOT_INSTALLED,
    F_BROKEN
} FilterMode;

typedef enum {
    ACT_NONE,
    ACT_INSTALL,
    ACT_REMOVE,
    ACT_REINSTALL,
    ACT_UPGRADE
} ActionMode;

enum {
    C_ACTION,
    C_STATE_ICON,
    C_NAME,
    C_INSTALLED,
    C_CANDIDATE,
    C_ARCH,
    C_BG,
    C_FG,
    C_HAS_COLOR,
    N_COLS
};

typedef struct {
    char *name;
    char *arch;
    char *installed;
    char *candidate;
    char *description;
    char *status;
    char *search_key;
    gboolean is_installed;
    gboolean is_upgradable;
    gboolean is_broken;
    ActionMode action;
    int candidate_priority;
    gboolean held;
} Package;

typedef struct {
    GtkWidget *window;
    GtkWidget *tree;
    GtkListStore *store;
    GPtrArray *sorted_packages;
    GtkWidget *search;
    GtkWidget *status_label;
    GtkWidget *repo_notice;
    GtkWidget *detail_name;
    GtkWidget *detail_desc;
    GtkWidget *detail_versions;
    GtkWidget *btn_install;
    GtkWidget *btn_remove;
    GtkWidget *btn_reinstall;
    GtkWidget *btn_clear;
    GtkWidget *btn_refresh;
    GtkWidget *btn_mark_upgrades;
    GtkWidget *btn_apply;
    GtkWidget *btn_fix;
    GtkWidget *btn_clean;
    FilterMode filter_mode;
    GHashTable *packages;
    gboolean busy;
    gboolean is_root;
    gboolean loading;
    gboolean catalog_loaded;
    gboolean repo_refreshed; /* successful explicit refresh in current session */
    BackendMode backend;
    char native_arch[64];
    char *search_folded;
    guint search_debounce_id;
    GdkPixbuf *icon_installed;
    GdkPixbuf *icon_broken;
    GdkPixbuf *icon_upgradable;
    GtkWidget *filter_buttons[5];

    GtkWidget *progress_dialog;
    GtkWidget *progress_bar;
    GtkWidget *progress_label;
    GtkWidget *progress_text;
    GtkTextBuffer *progress_buffer;
    GtkWidget *progress_close;
    GtkWidget *progress_scroller;
    GMutex progress_lock;
    GString *progress_pending;
    guint progress_flush_id;
    int pty_master;
    GString *pty_input;
    guint pulse_id;
    gboolean close_progress_after_load;
    gboolean refresh_queued;
    GThread *worker_thread;
    GThread *load_thread;
    GtkWidget *progress_cancel;
    gint active_child_pid;
    gint cancel_requested;
    gint cancel_locked;
    gint cancel_download_mode;   /* 1 while a real package command may still be downloading */
    gint cancel_download_seen;   /* set after a known-safe download line unlocked Cancel */
    gint edpkg_sim_capability; /* 0 unknown, 1 supported, -1 unsupported */
} App;

static const char *backend_name(const App *a);
static const char *backend_status_file(const App *a);
static const char *backend_lists_dir(const App *a);
static void progress_append(App *a, const char *text);
static void reload_data_async(App *a, gboolean force);

/* -------------------------------------------------------------------------
 * Small built-in i18n table.  Backend output intentionally remains in the
 * backend locale; only GUI chrome is translated here.
 * ------------------------------------------------------------------------- */

typedef enum {
    T_APP_TITLE, T_SUBTITLE, T_REFRESH, T_MARK_UPGRADES, T_APPLY, T_FIX,
    T_CLEAN, T_SEARCH, T_ALL, T_INSTALLED, T_UPGRADABLE, T_NOT_INSTALLED,
    T_BROKEN, T_ACTION, T_STATE, T_PACKAGE, T_INSTALLED_VER, T_CANDIDATE,
    T_ARCH, T_DESCRIPTION, T_MARK_INSTALL, T_MARK_REMOVE, T_MARK_REINSTALL,
    T_CLEAR_MARK, T_READY, T_NO_SELECTION, T_CLOSE, T_RUNNING, T_DONE,
    T_FAILED, T_CANCELLED, T_CONFIRM_APPLY, T_APPLY_TITLE, T_ROOT_REQUIRED,
    T_ROOT_REQUIRED_TEXT, T_FIX_TITLE, T_UPDATE_FIRST, T_AUTOREMOVE_AFTER,
    T_RUN_REPAIR, T_CANCEL, T_NO_MARKS, T_CLEAN_CONFIRM, T_ABOUT,
    T_REPOSITORIES, T_MULTI_SELECTED, T_MULTI_HELP,
    T_REPO_NOTICE, T_REFRESHING_REPOS, T_COUNT
} TrKey;

typedef struct { const char *code; const char *s[T_COUNT]; } Lang;

static const Lang langs[] = {
 {"en", {
  "EDPKG — Essora", "Package manager for Essora", "Refresh", "Mark upgrades", "Apply", "Fix broken",
  "Clean cache", "Search packages…", "All", "Installed", "Upgradable", "Not installed", "Broken",
  "Action", "State", "Package", "Installed", "Candidate", "Architecture", "Description", "Mark install",
  "Mark remove", "Mark reinstall", "Clear mark", "Ready", "Select a package to see details", "Close",
  "Running…", "Completed", "Operation failed", "Cancelled", "Apply the marked changes?", "Apply changes",
  "Administrator privileges required", "Package changes require root privileges. Browsing remains available.",
  "Repair broken packages", "Update repositories before repair", "Autoremove after repair", "Run repair",
  "Cancel", "There are no marked changes.", "Remove all cached package archives?", "About", "Repositories", "%u packages selected", "Ctrl-click selects individual packages; Shift-click selects a range. Actions apply to every compatible selected package.",
  "Installed packages are shown. Refresh repositories before installing or upgrading packages.", "Updating repositories…"
 }},
 {"es", {
  "EDPKG — Essora", "Gestor de paquetes para Essora", "Actualizar", "Marcar actualizaciones", "Aplicar", "Reparar rotos",
  "Limpiar caché", "Buscar paquetes…", "Todos", "Instalados", "Actualizables", "No instalados", "Rotos",
  "Acción", "Estado", "Paquete", "Instalada", "Candidata", "Arquitectura", "Descripción", "Marcar instalar",
  "Marcar eliminar", "Marcar reinstalar", "Quitar marca", "Listo", "Selecciona un paquete para ver detalles", "Cerrar",
  "Ejecutando…", "Completado", "La operación falló", "Cancelado", "¿Aplicar los cambios marcados?", "Aplicar cambios",
  "Se requieren privilegios de administrador", "Los cambios de paquetes requieren root. La consulta seguirá disponible.",
  "Reparar paquetes rotos", "Actualizar repositorios antes de reparar", "Autoremove después de reparar", "Reparar",
  "Cancelar", "No hay cambios marcados.", "¿Eliminar todos los paquetes guardados en caché?", "Acerca de", "Repositorios", "%u paquetes seleccionados", "Ctrl+clic selecciona paquetes individuales; Mayús+clic selecciona un rango. Las acciones se aplican a todos los paquetes seleccionados compatibles.",
  "Se muestran los paquetes instalados. Actualiza los repositorios antes de instalar o actualizar paquetes.", "Se están actualizando los repositorios…"
 }},
 {"fr", {
  "EDPKG — Essora", "Gestionnaire de paquets pour Essora", "Actualiser", "Marquer les mises à jour", "Appliquer", "Réparer",
  "Vider le cache", "Rechercher des paquets…", "Tous", "Installés", "Mises à jour", "Non installés", "Cassés",
  "Action", "État", "Paquet", "Installée", "Candidate", "Architecture", "Description", "Marquer installation",
  "Marquer suppression", "Marquer réinstallation", "Effacer la marque", "Prêt", "Sélectionnez un paquet", "Fermer",
  "Exécution…", "Terminé", "Échec de l’opération", "Annulé", "Appliquer les changements marqués ?", "Appliquer",
  "Privilèges administrateur requis", "Les modifications de paquets nécessitent root. La consultation reste disponible.",
  "Réparer les paquets cassés", "Mettre à jour les dépôts avant réparation", "Autoremove après réparation", "Réparer",
  "Annuler", "Aucun changement marqué.", "Supprimer toutes les archives du cache ?", "À propos", "Dépôts", "%u paquets sélectionnés", "Ctrl+clic sélectionne des paquets individuels ; Maj+clic sélectionne une plage. Les actions s’appliquent à tous les paquets sélectionnés compatibles.",
  "Les paquets installés sont affichés. Actualisez les dépôts avant d’installer ou de mettre à niveau des paquets.", "Mise à jour des dépôts en cours…"
 }},
 {"it", {
  "EDPKG — Essora", "Gestore pacchetti per Essora", "Aggiorna", "Segna aggiornamenti", "Applica", "Ripara",
  "Pulisci cache", "Cerca pacchetti…", "Tutti", "Installati", "Aggiornabili", "Non installati", "Danneggiati",
  "Azione", "Stato", "Pacchetto", "Installata", "Candidata", "Architettura", "Descrizione", "Segna installazione",
  "Segna rimozione", "Segna reinstallazione", "Rimuovi segno", "Pronto", "Seleziona un pacchetto", "Chiudi",
  "In esecuzione…", "Completato", "Operazione fallita", "Annullato", "Applicare le modifiche segnate?", "Applica modifiche",
  "Servono privilegi di amministratore", "Le modifiche ai pacchetti richiedono root. La consultazione resta disponibile.",
  "Ripara pacchetti danneggiati", "Aggiorna repository prima della riparazione", "Autoremove dopo la riparazione", "Ripara",
  "Annulla", "Non ci sono modifiche segnate.", "Eliminare tutti gli archivi nella cache?", "Informazioni", "Repository", "%u pacchetti selezionati", "Ctrl+clic seleziona singoli pacchetti; Maiusc+clic seleziona un intervallo. Le azioni si applicano a tutti i pacchetti selezionati compatibili.",
  "Sono mostrati i pacchetti installati. Aggiorna i repository prima di installare o aggiornare i pacchetti.", "Aggiornamento dei repository in corso…"
 }},
 {"pt", {
  "EDPKG — Essora", "Gerenciador de pacotes para Essora", "Atualizar", "Marcar atualizações", "Aplicar", "Reparar",
  "Limpar cache", "Pesquisar pacotes…", "Todos", "Instalados", "Atualizáveis", "Não instalados", "Quebrados",
  "Ação", "Estado", "Pacote", "Instalada", "Candidata", "Arquitetura", "Descrição", "Marcar instalar",
  "Marcar remover", "Marcar reinstalar", "Limpar marca", "Pronto", "Selecione um pacote", "Fechar",
  "Executando…", "Concluído", "A operação falhou", "Cancelado", "Aplicar as alterações marcadas?", "Aplicar alterações",
  "Privilégios de administrador necessários", "Alterações de pacotes exigem root. A consulta continua disponível.",
  "Reparar pacotes quebrados", "Atualizar repositórios antes de reparar", "Autoremove depois de reparar", "Reparar",
  "Cancelar", "Não há alterações marcadas.", "Remover todos os arquivos do cache?", "Sobre", "Repositórios", "%u pacotes selecionados", "Ctrl+clique seleciona pacotes individuais; Shift+clique seleciona um intervalo. As ações são aplicadas a todos os pacotes selecionados compatíveis.",
  "Os pacotes instalados são exibidos. Atualize os repositórios antes de instalar ou atualizar pacotes.", "Atualizando os repositórios…"
 }},
 {"ca", {
  "EDPKG — Essora", "Gestor de paquets per a Essora", "Actualitza", "Marca actualitzacions", "Aplica", "Repara",
  "Neteja memòria cau", "Cerca paquets…", "Tots", "Instal·lats", "Actualitzables", "No instal·lats", "Trencats",
  "Acció", "Estat", "Paquet", "Instal·lada", "Candidata", "Arquitectura", "Descripció", "Marca instal·lar",
  "Marca eliminar", "Marca reinstal·lar", "Treu la marca", "Llest", "Selecciona un paquet", "Tanca",
  "Executant…", "Completat", "L’operació ha fallat", "Cancel·lat", "Aplicar els canvis marcats?", "Aplica canvis",
  "Calen privilegis d’administrador", "Els canvis de paquets requereixen root. La consulta continua disponible.",
  "Repara paquets trencats", "Actualitza repositoris abans de reparar", "Autoremove després de reparar", "Repara",
  "Cancel·la", "No hi ha canvis marcats.", "Eliminar tots els arxius de la memòria cau?", "Quant a", "Repositoris", "%u paquets seleccionats", "Ctrl+clic selecciona paquets individuals; Maj+clic selecciona un interval. Les accions s’apliquen a tots els paquets seleccionats compatibles.",
  "Es mostren els paquets instal·lats. Actualitza els repositoris abans d’instal·lar o actualitzar paquets.", "S’estan actualitzant els repositoris…"
 }},
 {"de", {
  "EDPKG — Essora", "Paketverwaltung für Essora", "Aktualisieren", "Upgrades markieren", "Anwenden", "Reparieren",
  "Cache leeren", "Pakete suchen…", "Alle", "Installiert", "Aktualisierbar", "Nicht installiert", "Defekt",
  "Aktion", "Status", "Paket", "Installiert", "Kandidat", "Architektur", "Beschreibung", "Installation markieren",
  "Entfernen markieren", "Neuinstallation markieren", "Markierung löschen", "Bereit", "Paket auswählen", "Schließen",
  "Wird ausgeführt…", "Abgeschlossen", "Vorgang fehlgeschlagen", "Abgebrochen", "Markierte Änderungen anwenden?", "Änderungen anwenden",
  "Administratorrechte erforderlich", "Paketänderungen benötigen root. Lesen bleibt verfügbar.",
  "Defekte Pakete reparieren", "Repositorys vorher aktualisieren", "Autoremove danach ausführen", "Reparieren",
  "Abbrechen", "Keine Änderungen markiert.", "Alle Paketarchive im Cache löschen?", "Über", "Repositorys", "%u Pakete ausgewählt", "Strg+Klick wählt einzelne Pakete; Umschalt+Klick wählt einen Bereich. Aktionen gelten für alle passenden ausgewählten Pakete.",
  "Installierte Pakete werden angezeigt. Aktualisieren Sie die Paketquellen vor der Installation oder Aktualisierung.", "Paketquellen werden aktualisiert…"
 }},
 {"hu", {
  "EDPKG — Essora", "Essora csomagkezelő", "Frissítés", "Frissítések kijelölése", "Alkalmaz", "Javítás",
  "Gyorsítótár törlése", "Csomagok keresése…", "Mind", "Telepített", "Frissíthető", "Nincs telepítve", "Hibás",
  "Művelet", "Állapot", "Csomag", "Telepített", "Jelölt", "Architektúra", "Leírás", "Telepítés kijelölése",
  "Eltávolítás kijelölése", "Újratelepítés kijelölése", "Jelölés törlése", "Kész", "Válassz csomagot", "Bezárás",
  "Futtatás…", "Kész", "A művelet sikertelen", "Megszakítva", "Alkalmazza a kijelölt módosításokat?", "Módosítások alkalmazása",
  "Rendszergazdai jogosultság szükséges", "A csomagmódosításhoz root kell. A böngészés elérhető marad.",
  "Hibás csomagok javítása", "Tárolók frissítése javítás előtt", "Autoremove javítás után", "Javítás",
  "Mégse", "Nincs kijelölt módosítás.", "Törli az összes gyorsítótárazott csomagot?", "Névjegy", "Tárolók", "%u csomag kijelölve", "Ctrl+kattintás egyedi csomagokat, Shift+kattintás tartományt jelöl ki. A műveletek minden kompatibilis kijelölt csomagra érvényesek.",
  "A telepített csomagok láthatók. Telepítés vagy frissítés előtt frissítse a tárolókat.", "Tárolók frissítése folyamatban…"
 }},
 {"ru", {
  "EDPKG — Essora", "Менеджер пакетов Essora", "Обновить", "Отметить обновления", "Применить", "Исправить",
  "Очистить кэш", "Поиск пакетов…", "Все", "Установленные", "Обновляемые", "Не установлены", "Повреждённые",
  "Действие", "Состояние", "Пакет", "Установлена", "Кандидат", "Архитектура", "Описание", "Отметить установку",
  "Отметить удаление", "Отметить переустановку", "Снять отметку", "Готово", "Выберите пакет", "Закрыть",
  "Выполняется…", "Завершено", "Операция не удалась", "Отменено", "Применить отмеченные изменения?", "Применить изменения",
  "Нужны права администратора", "Для изменения пакетов нужны права root. Просмотр остаётся доступным.",
  "Исправить повреждённые пакеты", "Обновить репозитории перед исправлением", "Autoremove после исправления", "Исправить",
  "Отмена", "Нет отмеченных изменений.", "Удалить все архивы пакетов из кэша?", "О программе", "Репозитории", "Выбрано пакетов: %u", "Ctrl+щелчок выбирает отдельные пакеты; Shift+щелчок — диапазон. Действия применяются ко всем совместимым выбранным пакетам.",
  "Показаны установленные пакеты. Обновите репозитории перед установкой или обновлением пакетов.", "Идёт обновление репозиториев…"
 }},
 {"ja", {
  "EDPKG — Essora", "Essora パッケージマネージャー", "更新", "更新を選択", "適用", "修復",
  "キャッシュを消去", "パッケージを検索…", "すべて", "インストール済み", "更新可能", "未インストール", "破損",
  "操作", "状態", "パッケージ", "インストール済み", "候補", "アーキテクチャ", "説明", "インストールを選択",
  "削除を選択", "再インストールを選択", "選択解除", "準備完了", "パッケージを選択してください", "閉じる",
  "実行中…", "完了", "操作に失敗しました", "キャンセルされました", "選択した変更を適用しますか？", "変更を適用",
  "管理者権限が必要です", "パッケージの変更には root 権限が必要です。閲覧は利用できます。",
  "破損パッケージを修復", "修復前にリポジトリを更新", "修復後に autoremove", "修復",
  "キャンセル", "選択された変更はありません。", "キャッシュ内の全パッケージを削除しますか？", "このアプリについて", "リポジトリ", "%u 個のパッケージを選択", "Ctrl+クリックで個別選択、Shift+クリックで範囲選択します。操作は互換性のある選択済みパッケージすべてに適用されます。",
  "インストール済みパッケージを表示しています。インストールやアップグレードの前にリポジトリを更新してください。", "リポジトリを更新しています…"
 }},
 {"zh", {
  "EDPKG — Essora", "Essora 软件包管理器", "刷新", "标记升级", "应用", "修复损坏",
  "清理缓存", "搜索软件包…", "全部", "已安装", "可升级", "未安装", "损坏",
  "操作", "状态", "软件包", "已安装", "候选版本", "架构", "说明", "标记安装",
  "标记删除", "标记重装", "清除标记", "就绪", "选择软件包查看详情", "关闭",
  "正在运行…", "已完成", "操作失败", "已取消", "应用已标记的更改？", "应用更改",
  "需要管理员权限", "更改软件包需要 root 权限。仍可浏览。",
  "修复损坏的软件包", "修复前更新软件源", "修复后执行 autoremove", "运行修复",
  "取消", "没有已标记的更改。", "删除所有缓存的软件包归档？", "关于", "软件源", "已选择 %u 个软件包", "Ctrl+单击选择单个软件包；Shift+单击选择范围。操作会应用到所有兼容的已选软件包。",
  "当前显示已安装的软件包。安装或升级软件包前，请先刷新软件源。", "正在更新软件源…"
 }},
 {"ar", {
  "EDPKG — Essora", "مدير حزم Essora", "تحديث", "تحديد التحديثات", "تطبيق", "إصلاح",
  "تنظيف الذاكرة المؤقتة", "البحث عن حزم…", "الكل", "مثبت", "قابل للتحديث", "غير مثبت", "معطوب",
  "الإجراء", "الحالة", "الحزمة", "المثبت", "المرشح", "المعمارية", "الوصف", "تحديد للتثبيت",
  "تحديد للإزالة", "تحديد لإعادة التثبيت", "مسح التحديد", "جاهز", "اختر حزمة لعرض التفاصيل", "إغلاق",
  "جارٍ التنفيذ…", "اكتمل", "فشلت العملية", "تم الإلغاء", "تطبيق التغييرات المحددة؟", "تطبيق التغييرات",
  "صلاحيات المدير مطلوبة", "تغييرات الحزم تحتاج صلاحيات root. يبقى التصفح متاحاً.",
  "إصلاح الحزم المعطوبة", "تحديث المستودعات قبل الإصلاح", "Autoremove بعد الإصلاح", "إصلاح",
  "إلغاء", "لا توجد تغييرات محددة.", "حذف جميع أرشيفات الحزم المخزنة؟", "حول", "المستودعات", "تم تحديد %u حزمة", "Ctrl+نقرة لتحديد حزم منفردة، وShift+نقرة لتحديد نطاق. تُطبّق الإجراءات على كل الحزم المحددة المتوافقة.",
  "يتم عرض الحزم المثبتة. حدّث المستودعات قبل تثبيت الحزم أو ترقيتها.", "جارٍ تحديث المستودعات…"
 }}
};

static const Lang *L = &langs[0];
static const char *tr(TrKey k) { return L->s[k]; }

static void choose_language(void) {
    const char *e = g_getenv("LANG");
    char code[8] = "en";
    if (e && *e) {
        size_t n = strcspn(e, "_.@");
        if (n > 0 && n < sizeof(code)) {
            memcpy(code, e, n); code[n] = 0;
            for (size_t i=0; code[i]; i++) code[i] = (char)g_ascii_tolower(code[i]);
        }
    }
    for (guint i=0; i<G_N_ELEMENTS(langs); i++) if (!strcmp(code, langs[i].code)) { L=&langs[i]; break; }
}

static void package_free(gpointer p_) {
    Package *p = p_;
    if (!p) return;
    g_free(p->name); g_free(p->arch); g_free(p->installed); g_free(p->candidate);
    g_free(p->description); g_free(p->status); g_free(p->search_key); g_free(p);
}

/*
 * Essora intentionally keeps kernel lifecycle outside EDPKG GUI.
 * These package families are neither listed nor accepted for install,
 * reinstall or upgrade actions from this application.  Keep linux-libc-dev,
 * linux-perf, firmware packages and other userspace tools visible: they are
 * not kernel images/modules.
 */
static gboolean package_is_kernel_blocked(const char *name) {
    if (!name || !*name) return FALSE;
    static const char *families[] = {
        "linux-image", "linux-kbuild", "linux-headers", "linux-modules",
        "linux-source", "linux-config", "linux-compiler", "linux-support", "linux-doc",
        "kernel-image", "kernel-headers", "kernel-modules", NULL
    };
    for (guint i = 0; families[i]; i++) {
        const size_t n = strlen(families[i]);
        if (strncmp(name, families[i], n) == 0 &&
            (name[n] == '\0' || name[n] == '-'))
            return TRUE;
    }
    return FALSE;
}

static Package *package_new(const char *name) {
    Package *p = g_new0(Package, 1);
    p->name = g_strdup(name ? name : "");
    p->arch = g_strdup("");
    p->installed = g_strdup("");
    p->candidate = g_strdup("");
    p->description = g_strdup("");
    p->status = g_strdup("--");
    p->search_key = g_strdup("");
    return p;
}

static gint package_name_cmp(gconstpointer aa, gconstpointer bb);
static void set_string(char **dst, const char *src);
static void refresh_details(App *a);

static void package_refresh_search_key(Package *p) {
    if (!p) return;
    gchar *n = g_utf8_strdown(p->name ? p->name : "", -1);
    gchar *d = g_utf8_strdown(p->description ? p->description : "", -1);
    g_free(p->search_key);
    p->search_key = g_strconcat(n ? n : "", "\n", d ? d : "", NULL);
    g_free(n); g_free(d);
}

static void checksum_meta(GChecksum *sum, const char *path) {
    GStatBuf st;
    if (g_stat(path, &st) != 0) {
        g_checksum_update(sum, (const guchar*)path, strlen(path));
        g_checksum_update(sum, (const guchar*)":missing\n", 9);
        return;
    }
    gchar *m = g_strdup_printf("%s|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "\n",
                               path, (gint64)st.st_size, (gint64)st.st_mtime);
    g_checksum_update(sum, (const guchar*)m, strlen(m));
    g_free(m);
}

static gint str_ptr_cmp(gconstpointer aa, gconstpointer bb) {
    const char *a = *(char * const *)aa;
    const char *b = *(char * const *)bb;
    return g_strcmp0(a, b);
}

static void checksum_dir_meta(GChecksum *sum, const char *dir_path) {
    GDir *d = g_dir_open(dir_path, 0, NULL);
    if (!d) { checksum_meta(sum, dir_path); return; }
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const gchar *name;
    while ((name = g_dir_read_name(d))) {
        if (name[0] == '.') continue;
        g_ptr_array_add(names, g_strdup(name));
    }
    g_dir_close(d);
    g_ptr_array_sort(names, str_ptr_cmp);
    for (guint i = 0; i < names->len; i++) {
        gchar *path = g_build_filename(dir_path, g_ptr_array_index(names, i), NULL);
        checksum_meta(sum, path);
        g_free(path);
    }
    g_ptr_array_free(names, TRUE);
}

static gboolean apt_index_name(const char *name) {
    return name && (g_str_has_suffix(name, "_Packages") ||
                    g_str_has_suffix(name, "_Packages.lz4") ||
                    g_str_has_suffix(name, "_Packages.gz") ||
                    g_str_has_suffix(name, "_Packages.xz") ||
                    g_str_has_suffix(name, "_Packages.bz2"));
}

static gboolean edpkg_index_name(const char *name) {
    return name && (g_str_has_suffix(name, ".Packages") ||
                    g_str_has_suffix(name, ".Packages.base") ||
                    g_str_has_suffix(name, ".Packages.priority"));
}

static gchar *cache_signature(App *a) {
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    const char *lists_dir=backend_lists_dir(a);
    /* The persistent catalog cache represents repository metadata only.
     * Do NOT include /var/lib/dpkg/status (or EDPKG status) in this signature:
     * local installed state is overlaid on every startup so packages installed
     * from a terminal appear immediately without rebuilding the 70k catalog. */
    if (a->backend==BACKEND_EDPKG) checksum_meta(sum, edpkg_bin_path());
    else {
        checksum_meta(sum, APTGET_BIN);
        checksum_meta(sum, APTCACHE_BIN);
        checksum_meta(sum, DPKG_BIN);
        checksum_meta(sum, "/etc/apt/preferences");
        checksum_meta(sum, "/etc/apt/sources.list");
        checksum_dir_meta(sum, "/etc/apt/preferences.d");
        checksum_dir_meta(sum, "/etc/apt/sources.list.d");
    }

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(lists_dir, 0, NULL);
    if (d) {
        const gchar *name;
        while ((name = g_dir_read_name(d))) {
            gboolean take=a->backend==BACKEND_APT ? apt_index_name(name) : edpkg_index_name(name);
            if (take) g_ptr_array_add(names, g_strdup(name));
        }
        g_dir_close(d);
    }
    g_ptr_array_sort(names, str_ptr_cmp);
    for (guint i=0; i<names->len; i++) {
        gchar *path = g_build_filename(lists_dir, g_ptr_array_index(names, i), NULL);
        checksum_meta(sum, path);
        g_free(path);
    }
    g_ptr_array_free(names, TRUE);

    gchar *ret = g_strdup(g_checksum_get_string(sum));
    g_checksum_free(sum);
    return ret;
}

static gchar *cache_path_for_write(App *a) {
    gchar *base=g_strdup_printf("packages-v5-%s.tsv", a->backend==BACKEND_APT?"apt":"edpkg");
    if (g_mkdir_with_parents(CACHE_DIR, 0755) == 0 && g_access(CACHE_DIR, W_OK) == 0) { gchar *p=g_build_filename(CACHE_DIR,base,NULL); g_free(base); return p; }
    gchar *dir = g_build_filename(g_get_user_cache_dir(), "edpkg-gui", NULL);
    if (g_mkdir_with_parents(dir, 0755) != 0) { g_free(base); g_free(dir); return NULL; }
    gchar *path = g_build_filename(dir, base, NULL);
    g_free(base); g_free(dir);
    return path;
}

static gchar *cache_path_for_read(App *a) {
    gchar *base=g_strdup_printf("packages-v5-%s.tsv", a->backend==BACKEND_APT?"apt":"edpkg");
    gchar *path=g_build_filename(CACHE_DIR,base,NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) { g_free(base); return path; }
    g_free(path);
    path = g_build_filename(g_get_user_cache_dir(), "edpkg-gui", base, NULL);
    g_free(base);
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) { g_free(path); return NULL; }
    return path;
}

static gchar *cache_field(const char *s) {
    gchar *r = g_strdup(s ? s : "");
    for (gchar *p=r; *p; p++) if (*p=='\t' || *p=='\n' || *p=='\r') *p=' ';
    return r;
}

static gboolean save_package_cache(App *a) {
    gchar *path = cache_path_for_write(a);
    if (!path) return FALSE;
    gchar *sig = cache_signature(a);
    GString *out = g_string_new("# edpkg-gui-cache-v5\n");
    g_string_append_printf(out, "# backend=%s\n", backend_name(a));
    g_string_append_printf(out, "# signature=%s\n# arch=%s\n", sig, a->native_arch);

    GList *vals = g_hash_table_get_values(a->packages);
    vals = g_list_sort(vals, package_name_cmp);
    for (GList *l=vals; l; l=l->next) {
        Package *p=l->data;
        gchar *name=cache_field(p->name), *arch=cache_field(p->arch);
        gchar *cand=cache_field(p->candidate), *desc=cache_field(p->description);
        g_string_append_printf(out, "%s\t%s\t%s\t%d\t%s\n",
                               name, arch, cand, p->candidate_priority, desc);
        g_free(name); g_free(arch); g_free(cand); g_free(desc);
    }
    g_list_free(vals);

    gchar *tmp = g_strdup_printf("%s.tmp.%ld", path, (long)getpid());
    gboolean ok = g_file_set_contents(tmp, out->str, out->len, NULL);
    if (ok) {
        if (g_rename(tmp, path) != 0) { g_unlink(tmp); ok=FALSE; }
        else g_chmod(path, 0644);
    }
    g_free(tmp); g_string_free(out, TRUE); g_free(sig); g_free(path);
    return ok;
}

static gboolean load_package_cache(App *a) {
    gchar *path = cache_path_for_read(a);
    if (!path) return FALSE;
    gchar *text=NULL; gsize len=0;
    if (!g_file_get_contents(path, &text, &len, NULL)) { g_free(path); return FALSE; }
    g_free(path);

    gchar **lines = g_strsplit(text, "\n", -1);
    if (!lines[0] || strcmp(lines[0], "# edpkg-gui-cache-v5")) { g_strfreev(lines); g_free(text); return FALSE; }
    const char *cached_backend = lines[1] && g_str_has_prefix(lines[1], "# backend=") ? lines[1]+10 : "";
    if (strcmp(cached_backend, backend_name(a))) { g_strfreev(lines); g_free(text); return FALSE; }
    const char *cached_sig = lines[2] && g_str_has_prefix(lines[2], "# signature=") ? lines[2]+12 : "";
    gchar *sig = cache_signature(a);
    if (strcmp(cached_sig, sig)) { g_free(sig); g_strfreev(lines); g_free(text); return FALSE; }
    g_free(sig);
    if (lines[3] && g_str_has_prefix(lines[3], "# arch="))
        g_strlcpy(a->native_arch, lines[3]+7, sizeof(a->native_arch));

    for (int i=4; lines[i]; i++) {
        if (!*lines[i]) continue;
        gchar **f = g_strsplit(lines[i], "\t", 5);
        if (g_strv_length(f) != 5) { g_strfreev(f); continue; }
        if (package_is_kernel_blocked(f[0])) { g_strfreev(f); continue; }
        Package *p = package_new(f[0]);
        set_string(&p->arch, f[1]); set_string(&p->candidate, f[2]);
        p->candidate_priority=atoi(f[3]); set_string(&p->description, f[4]); package_refresh_search_key(p);
        g_hash_table_insert(a->packages, g_strdup(p->name), p);
        g_strfreev(f);
    }
    g_strfreev(lines); g_free(text);
    return g_hash_table_size(a->packages) > 0;
}

static gchar *capture_argv(gchar **argv, gint *exit_status) {
    gchar *out = NULL, *err = NULL;
    gint st = 0;
    GError *e = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &st, &e);
    if (!ok) {
        if (exit_status) *exit_status = 127;
        if (e) { g_printerr("edpkg-gui: %s\n", e->message); g_error_free(e); }
        g_free(out); out = g_strdup("");
    } else {
        if (exit_status) *exit_status = (WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    }
    if (err && *err) g_printerr("%s", err);
    g_free(err);
    return out ? out : g_strdup("");
}

/* Dependency simulations must be cancellable too.  g_spawn_sync() hides the
 * child PID, which made the Cancel button useless if a resolver got stuck.
 * Spawn the simulation in its own process group, publish its PID to the GUI,
 * capture stdout/stderr, and use a C locale because these plan lines are
 * machine-parsed rather than shown as normal backend output. */
static void guard_child_setup(gpointer data){
    (void)data;
    (void)setpgid(0,0);
}

static void capture_fd_nonblock(int fd,GString *dst,gboolean *open_flag){
    char buf[4096];
    while(*open_flag){
        ssize_t n=read(fd,buf,sizeof(buf));
        if(n>0){g_string_append_len(dst,buf,(gssize)n);continue;}
        if(n==0){close(fd);*open_flag=FALSE;break;}
        if(errno==EINTR)continue;
        if(errno==EAGAIN||errno==EWOULDBLOCK)break;
        close(fd);*open_flag=FALSE;break;
    }
}

static gchar *capture_guard_argv(App *a,gchar **argv,gboolean argv0_mode,
                                 gint *exit_status,gchar **stderr_text){
    if(stderr_text)*stderr_text=NULL;
    GError *err=NULL;GPid pid=0;gint outfd=-1,errfd=-1;
    GSpawnFlags flags=G_SPAWN_DO_NOT_REAP_CHILD;
    if(argv0_mode)flags|=G_SPAWN_FILE_AND_ARGV_ZERO;
    gchar **env=g_get_environ();
    env=g_environ_setenv(env,"LC_ALL","C",TRUE);
    env=g_environ_setenv(env,"LANG","C",TRUE);
    gboolean ok=g_spawn_async_with_pipes(NULL,argv,env,flags,guard_child_setup,NULL,
                                         &pid,NULL,&outfd,&errfd,&err);
    g_strfreev(env);
    if(!ok){
        if(exit_status)*exit_status=127;
        if(stderr_text)*stderr_text=g_strdup(err?err->message:"unable to start simulation");
        if(err)g_error_free(err);
        return g_strdup("");
    }
    int fl=fcntl(outfd,F_GETFL,0);if(fl>=0)(void)fcntl(outfd,F_SETFL,fl|O_NONBLOCK);
    fl=fcntl(errfd,F_GETFL,0);if(fl>=0)(void)fcntl(errfd,F_SETFL,fl|O_NONBLOCK);
    g_atomic_int_set(&a->active_child_pid,(gint)pid);
    if(g_atomic_int_get(&a->cancel_requested)){
        if(kill(-(pid_t)pid,SIGINT)!=0)(void)kill((pid_t)pid,SIGINT);
    }

    GString *out=g_string_new(""),*errs=g_string_new("");
    gboolean out_open=TRUE,err_open=TRUE,child_done=FALSE;
    gint status=0;gint64 cancel_since=0;gboolean term_sent=FALSE,kill_sent=FALSE;
    while(out_open||err_open||!child_done){
        fd_set rfds;FD_ZERO(&rfds);int maxfd=-1;
        if(out_open){FD_SET(outfd,&rfds);if(outfd>maxfd)maxfd=outfd;}
        if(err_open){FD_SET(errfd,&rfds);if(errfd>maxfd)maxfd=errfd;}
        struct timeval tv={0,100000};
        int sel=0;
        if(maxfd>=0)sel=select(maxfd+1,&rfds,NULL,NULL,&tv);
        else g_usleep(50000);
        if(sel<0&&errno==EINTR)continue;
        if(sel>0){
            if(out_open&&FD_ISSET(outfd,&rfds))capture_fd_nonblock(outfd,out,&out_open);
            if(err_open&&FD_ISSET(errfd,&rfds))capture_fd_nonblock(errfd,errs,&err_open);
        }
        if(!child_done){
            pid_t q=waitpid((pid_t)pid,&status,WNOHANG);
            if(q==(pid_t)pid)child_done=TRUE;
        }
        if(g_atomic_int_get(&a->cancel_requested)&&!child_done){
            gint64 now=g_get_monotonic_time();
            if(!cancel_since){
                cancel_since=now;
                if(kill(-(pid_t)pid,SIGINT)!=0)(void)kill((pid_t)pid,SIGINT);
            }else if(!term_sent&&now-cancel_since>3000000){
                term_sent=TRUE;
                if(kill(-(pid_t)pid,SIGTERM)!=0)(void)kill((pid_t)pid,SIGTERM);
            }else if(!kill_sent&&now-cancel_since>6000000){
                kill_sent=TRUE;
                if(kill(-(pid_t)pid,SIGKILL)!=0)(void)kill((pid_t)pid,SIGKILL);
            }
        }
        if(child_done){
            if(out_open)capture_fd_nonblock(outfd,out,&out_open);
            if(err_open)capture_fd_nonblock(errfd,errs,&err_open);
        }
    }
    if(!child_done)(void)waitpid((pid_t)pid,&status,0);
    g_atomic_int_set(&a->active_child_pid,0);
    g_spawn_close_pid(pid);
    gint rc=1;
    if(g_atomic_int_get(&a->cancel_requested))rc=130;
    else if(WIFEXITED(status))rc=WEXITSTATUS(status);
    else if(WIFSIGNALED(status))rc=128+WTERMSIG(status);
    if(exit_status)*exit_status=rc;
    if(stderr_text)*stderr_text=g_string_free(errs,FALSE);else g_string_free(errs,TRUE);
    return g_string_free(out,FALSE);
}

static BackendMode detect_backend(void) {
    if (g_file_test(edpkg_bin_path(), G_FILE_TEST_IS_EXECUTABLE)) return BACKEND_EDPKG;
    if (g_file_test(APTGET_BIN, G_FILE_TEST_IS_EXECUTABLE) &&
        g_file_test(APTCACHE_BIN, G_FILE_TEST_IS_EXECUTABLE) &&
        g_file_test(DPKG_BIN, G_FILE_TEST_IS_EXECUTABLE)) return BACKEND_APT;
    return BACKEND_NONE;
}

static const char *backend_name(const App *a) {
    return a->backend==BACKEND_EDPKG ? "EDPKG" : a->backend==BACKEND_APT ? "APT/dpkg" : "none";
}

static const char *backend_status_file(const App *a) {
    return a->backend==BACKEND_APT ? DPKG_STATUS_FILE : EDPKG_STATUS_FILE;
}

static const char *backend_lists_dir(const App *a) {
    return a->backend==BACKEND_APT ? APT_LISTS_DIR : EDPKG_LISTS_DIR;
}

static void set_string(char **dst, const char *src) { g_free(*dst); *dst = g_strdup(src ? src : ""); }

static void load_native_arch(App *a) {
    char *argv_edpkg[] = { (char*)edpkg_bin_path(), "--print-architecture", NULL };
    char *argv_dpkg[] = { (char*)DPKG_BIN, "--print-architecture", NULL };
    char **argv = a->backend==BACKEND_APT ? argv_dpkg : argv_edpkg;
    gint rc=0; gchar *out=capture_argv(argv,&rc);
    g_strstrip(out);
    g_strlcpy(a->native_arch, (rc==0 && *out) ? out : "amd64", sizeof(a->native_arch));
    g_free(out);
}

/* Fast local index reader.
 *
 * Synaptic stays responsive because it does not spawn apt commands for every
 * view: it works from an already parsed package cache.  EDPKG GUI follows the
 * same principle here. On EDPKG we read its status/Packages databases directly.
 * On classic APT/dpkg we read /var/lib/dpkg/status directly and run one
 * `apt-cache dumpavail` in the background so APT itself handles compressed
 * indexes and pinning/candidate selection. No command is spawned per row.
 */

static int version_order_char(unsigned char c) {
    if (c == '~') return -1;
    if (c == 0 || g_ascii_isdigit(c)) return 0;
    if (g_ascii_isalpha(c)) return c;
    return c + 256;
}

static int version_cmp_part(const char *a, const char *b) {
    size_t i=0,j=0,la=strlen(a?a:""),lb=strlen(b?b:"");
    a=a?a:""; b=b?b:"";
    while (i<la || j<lb) {
        int first=0;
        while ((i<la && !g_ascii_isdigit(a[i])) || (j<lb && !g_ascii_isdigit(b[j]))) {
            int ac=(i<la && !g_ascii_isdigit(a[i])) ? version_order_char((unsigned char)a[i]) : 0;
            int bc=(j<lb && !g_ascii_isdigit(b[j])) ? version_order_char((unsigned char)b[j]) : 0;
            if (ac != bc) return ac < bc ? -1 : 1;
            if (i<la && !g_ascii_isdigit(a[i])) i++;
            if (j<lb && !g_ascii_isdigit(b[j])) j++;
        }
        while (i<la && a[i]=='0') i++;
        while (j<lb && b[j]=='0') j++;
        size_t ia=i,jb=j;
        while (i<la && g_ascii_isdigit(a[i])) i++;
        while (j<lb && g_ascii_isdigit(b[j])) j++;
        size_t na=i-ia,nb=j-jb;
        if (na != nb) return na < nb ? -1 : 1;
        for (size_t k=0;k<na;k++) if (!first && a[ia+k]!=b[jb+k]) first=(unsigned char)a[ia+k]-(unsigned char)b[jb+k];
        if (first) return first<0 ? -1 : 1;
    }
    return 0;
}

static void version_split(const char *v, long *epoch, char **up, char **rev) {
    const char *s=v?v:"", *colon=strchr(s, ':'), *dash;
    *epoch=0;
    if (colon) {
        gboolean ok=TRUE;
        for (const char *q=s;q<colon;q++) if (!g_ascii_isdigit(*q)) ok=FALSE;
        if (ok) {
            gchar *e=g_strndup(s,(gsize)(colon-s));
            *epoch=strtol(e,NULL,10); g_free(e); s=colon+1;
        }
    }
    dash=strrchr(s,'-');
    if (dash) { *up=g_strndup(s,(gsize)(dash-s)); *rev=g_strdup(dash+1); }
    else { *up=g_strdup(s); *rev=g_strdup(""); }
}

static int version_compare_local(const char *a, const char *b) {
    long ea,eb; char *ua,*ub,*ra,*rb;
    version_split(a,&ea,&ua,&ra); version_split(b,&eb,&ub,&rb);
    int r=0;
    if (ea!=eb) r=ea<eb?-1:1;
    else if ((r=version_cmp_part(ua,ub))==0) r=version_cmp_part(ra,rb);
    g_free(ua);g_free(ub);g_free(ra);g_free(rb);return r;
}

static int packages_file_priority(const char *path) {
    gchar *pp=g_strdup_printf("%s.priority",path), *txt=NULL; gsize n=0;
    int pri=500;
    if (g_file_get_contents(pp,&txt,&n,NULL) && txt) {
        g_strstrip(txt); int p=atoi(txt); if (p>0 && p<=10000) pri=p;
    } else {
        gchar *bn=g_path_get_basename(path);
        if (strstr(bn,"backports")) pri=100;
        else if (strstr(bn,"xlibre-debian.github.io")) pri=700;
        g_free(bn);
    }
    g_free(txt);g_free(pp);return pri;
}

static gboolean arch_is_visible(App *a, const char *arch) {
    return arch && (!strcmp(arch,"all") || !strcmp(arch,a->native_arch));
}

typedef struct {
    gchar *name,*version,*arch,*description;
} FastStanza;

static void fast_stanza_clear(FastStanza *s) {
    g_free(s->name);g_free(s->version);g_free(s->arch);g_free(s->description);
    memset(s,0,sizeof(*s));
}

static void fast_field_set(gchar **dst,const char *value) {
    if (*dst) return;
    gchar *v=g_strdup(value?value:""); g_strstrip(v); *dst=v;
}

static void accept_repo_stanza(App *a, FastStanza *st, int pri) {
    if (!st->name || !*st->name || !st->version || !*st->version || !arch_is_visible(a,st->arch)) return;
    if (package_is_kernel_blocked(st->name)) return;
    Package *p=g_hash_table_lookup(a->packages,st->name);
    if(!p){p=package_new(st->name);g_hash_table_insert(a->packages,g_strdup(st->name),p);}
    gboolean better=!p->candidate[0] || pri>p->candidate_priority ||
        (pri==p->candidate_priority && version_compare_local(st->version,p->candidate)>0);
    if (better) {
        set_string(&p->candidate,st->version);
        p->candidate_priority=pri;
        if (!p->is_installed && st->arch && *st->arch) set_string(&p->arch,st->arch);
        if (st->description && *st->description) set_string(&p->description,st->description);
    }
}

static void parse_packages_file_fast(App *a,const char *path,int pri) {
    FILE *f=fopen(path,"r"); if(!f)return;
    char *line=NULL;size_t cap=0;FastStanza st={0};
    while(getline(&line,&cap,f)>0){
        g_strchomp(line);
        if(!*line){accept_repo_stanza(a,&st,pri);fast_stanza_clear(&st);continue;}
        if(g_str_has_prefix(line,"Package:")) fast_field_set(&st.name,line+8);
        else if(g_str_has_prefix(line,"Version:")) fast_field_set(&st.version,line+8);
        else if(g_str_has_prefix(line,"Architecture:")) fast_field_set(&st.arch,line+13);
        else if(g_str_has_prefix(line,"Description:")) fast_field_set(&st.description,line+12);
    }
    accept_repo_stanza(a,&st,pri);fast_stanza_clear(&st);free(line);fclose(f);
}

static void parse_packages_stream_fast(App *a, FILE *f, int pri) {
    if(!f)return;
    char *line=NULL;size_t cap=0;FastStanza st={0};
    while(getline(&line,&cap,f)>0){
        g_strchomp(line);
        if(!*line){accept_repo_stanza(a,&st,pri);fast_stanza_clear(&st);continue;}
        if(g_str_has_prefix(line,"Package:")) fast_field_set(&st.name,line+8);
        else if(g_str_has_prefix(line,"Version:")) fast_field_set(&st.version,line+8);
        else if(g_str_has_prefix(line,"Architecture:")) fast_field_set(&st.arch,line+13);
        else if(g_str_has_prefix(line,"Description:")) fast_field_set(&st.description,line+12);
    }
    accept_repo_stanza(a,&st,pri);fast_stanza_clear(&st);free(line);
}

static guint parse_apt_available_fast(App *a) {
    FILE *f=popen(APTCACHE_BIN " dumpavail 2>/dev/null","r");
    if(!f)return 0;
    parse_packages_stream_fast(a,f,500);
    int rc=pclose(f);
    return rc==0 ? 1 : 0;
}

static guint parse_edpkg_available_fast(App *a) {
    GDir*d=g_dir_open(EDPKG_LISTS_DIR,0,NULL); if(!d)return 0;
    GPtrArray*files=g_ptr_array_new_with_free_func(g_free);const gchar*name;
    while((name=g_dir_read_name(d))) if(g_str_has_suffix(name,".Packages")) g_ptr_array_add(files,g_strdup(name));
    g_dir_close(d);g_ptr_array_sort(files,str_ptr_cmp);
    for(guint i=0;i<files->len;i++){
        gchar*path=g_build_filename(EDPKG_LISTS_DIR,g_ptr_array_index(files,i),NULL);
        int pri=packages_file_priority(path);parse_packages_file_fast(a,path,pri);g_free(path);
    }
    guint n=files->len;g_ptr_array_free(files,TRUE);return n;
}

static guint parse_available_fast(App *a) {
    return a->backend==BACKEND_APT ? parse_apt_available_fast(a) : parse_edpkg_available_fast(a);
}

static void accept_status_stanza(App *a,FastStanza *st,const char *status,gboolean held) {
    if(!st->name||!*st->name||!st->version||!*st->version)return;
    Package*p=g_hash_table_lookup(a->packages,st->name);
    if(!p){p=package_new(st->name);g_hash_table_insert(a->packages,g_strdup(st->name),p);}
    set_string(&p->installed,st->version);
    if(st->arch&&*st->arch)set_string(&p->arch,st->arch);
    if(st->description&&*st->description&&!p->description[0])set_string(&p->description,st->description);
    set_string(&p->status,status?status:"");p->held=held;
    const char*last=status?strrchr(status,' '):NULL;last=last?last+1:(status?status:"");
    p->is_installed=!strcmp(last,"installed")||!strcmp(last,"unpacked")||!strcmp(last,"half-configured")||
        !strcmp(last,"half-installed")||!strcmp(last,"triggers-awaited")||!strcmp(last,"triggers-pending");
    p->is_broken=!strcmp(last,"unpacked")||!strcmp(last,"half-configured")||!strcmp(last,"half-installed")||
        !strcmp(last,"triggers-awaited")||!strcmp(last,"triggers-pending")||(!*last?FALSE:(!strcmp(last,"unknown")));
    if(a->backend==BACKEND_APT&&status&&*status){
        gchar **parts=g_strsplit_set(status," \t",-1);
        GPtrArray *words=g_ptr_array_new();
        for(guint i=0;parts[i];i++)if(*parts[i])g_ptr_array_add(words,parts[i]);
        if(words->len>=1&&!strcmp(g_ptr_array_index(words,0),"hold"))p->held=TRUE;
        if(words->len>=2&&strcmp(g_ptr_array_index(words,1),"ok"))p->is_broken=TRUE;
        g_ptr_array_free(words,TRUE);g_strfreev(parts);
    }
}

static void clear_local_package_state(App *a) {
    GHashTableIter it; gpointer key, value;
    g_hash_table_iter_init(&it, a->packages);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        Package *p = value;
        set_string(&p->installed, "");
        set_string(&p->status, "");
        p->is_installed = FALSE;
        p->is_upgradable = FALSE;
        p->is_broken = FALSE;
        p->held = FALSE;
        p->action = ACT_NONE;
    }
}

static guint parse_status_fast(App *a) {
    FILE*f=fopen(backend_status_file(a),"r");if(!f)return 0;
    char*line=NULL;size_t cap=0;FastStanza st={0};gchar*status=NULL;gboolean held=FALSE;guint count=0;
    while(getline(&line,&cap,f)>0){
        g_strchomp(line);
        if(!*line){if(st.name){accept_status_stanza(a,&st,status,held);count++;}fast_stanza_clear(&st);g_clear_pointer(&status,g_free);held=FALSE;continue;}
        if(g_str_has_prefix(line,"Package:")) fast_field_set(&st.name,line+8);
        else if(g_str_has_prefix(line,"Version:")) fast_field_set(&st.version,line+8);
        else if(g_str_has_prefix(line,"Architecture:")) fast_field_set(&st.arch,line+13);
        else if(g_str_has_prefix(line,"Description:")) fast_field_set(&st.description,line+12);
        else if(g_str_has_prefix(line,"Status:")){if(!status){status=g_strdup(line+7);g_strstrip(status);if(a->backend==BACKEND_APT&&g_str_has_prefix(status,"hold "))held=TRUE;}}
        else if(g_str_has_prefix(line,"Hold:")){gchar*v=g_strdup(line+5);g_strstrip(v);held=!g_ascii_strcasecmp(v,"yes")||!strcmp(v,"1")||!g_ascii_strcasecmp(v,"true");g_free(v);}
    }
    if(st.name){accept_status_stanza(a,&st,status,held);count++;}
    fast_stanza_clear(&st);g_free(status);free(line);fclose(f);return count;
}

static void recompute_upgrade_flags(App *a) {
    GHashTableIter hi; gpointer k, v;
    g_hash_table_iter_init(&hi, a->packages);
    while (g_hash_table_iter_next(&hi, &k, &v)) {
        Package *p = v;
        p->is_upgradable = p->is_installed && !p->held && p->candidate[0] && p->installed[0] &&
            version_compare_local(p->candidate, p->installed) > 0;
    }
}

static void refresh_all_search_keys(App *a) {
    GHashTableIter hi; gpointer k, v;
    g_hash_table_iter_init(&hi, a->packages);
    while (g_hash_table_iter_next(&hi, &k, &v))
        package_refresh_search_key(v);
}

static void finalize_package_states(App *a) {
    recompute_upgrade_flags(a);
    refresh_all_search_keys(a);
}

static void overlay_apt_policy_candidates(App *a) {
    if (!a || a->backend != BACKEND_APT || !g_file_test(APT_BIN, G_FILE_TEST_IS_EXECUTABLE)) return;
    /* `apt list` is generated from APT's own cache/policy, so it honours pins,
     * source priorities and preferences.  It is intentionally run only during
     * explicit Refresh; the resulting candidate versions are persisted in the
     * repository cache and normal startup remains local-only. */
    FILE *f=popen(APT_BIN " list 2>/dev/null", "r");
    if (!f) return;
    char *line=NULL; size_t cap=0;
    while (getline(&line,&cap,f)>0) {
        g_strchomp(line);
        if (!*line || g_str_has_prefix(line,"Listing")) continue;
        gchar **parts=g_strsplit_set(line," 	",0);
        GPtrArray *words=g_ptr_array_new();
        for (guint i=0; parts[i]; i++) if (*parts[i]) g_ptr_array_add(words, parts[i]);
        if (words->len >= 3) {
            gchar *namesuite=g_ptr_array_index(words,0);
            const gchar *version=g_ptr_array_index(words,1);
            const gchar *arch=g_ptr_array_index(words,2);
            gchar *slash=strchr(namesuite,'/');
            gchar *name=slash?g_strndup(namesuite,(gsize)(slash-namesuite)):g_strdup(namesuite);
            gchar *colon=strchr(name,':'); if(colon)*colon='\0';
            Package *p=g_hash_table_lookup(a->packages,name);
            if (p && arch_is_visible(a,arch) && !package_is_kernel_blocked(p->name)) {
                set_string(&p->candidate,version);
                p->candidate_priority=1000; /* selected by APT policy, not guessed */
            }
            g_free(name);
        }
        g_ptr_array_free(words,TRUE); g_strfreev(parts);
    }
    free(line); (void)pclose(f);
}

static const char *action_text(ActionMode a) {
    switch(a){
        case ACT_INSTALL:return tr(T_MARK_INSTALL);
        case ACT_REMOVE:return tr(T_MARK_REMOVE);
        case ACT_REINSTALL:return tr(T_MARK_REINSTALL);
        case ACT_UPGRADE:return tr(T_UPGRADABLE);
        default:return "";
    }
}

static gint package_name_cmp(gconstpointer aa, gconstpointer bb) {
    const Package *a=aa,*b=bb; return g_ascii_strcasecmp(a->name,b->name);
}

static gint package_ptr_cmp(gconstpointer aa, gconstpointer bb) {
    const Package *a = *(Package * const *)aa;
    const Package *b = *(Package * const *)bb;
    return g_ascii_strcasecmp(a->name, b->name);
}

static void rebuild_sorted_packages(App *a) {
    if (a->sorted_packages) g_ptr_array_free(a->sorted_packages, TRUE);
    a->sorted_packages = g_ptr_array_new();
    GHashTableIter it; gpointer key, value;
    g_hash_table_iter_init(&it, a->packages);
    while (g_hash_table_iter_next(&it, &key, &value)) g_ptr_array_add(a->sorted_packages, value);
    g_ptr_array_sort(a->sorted_packages, package_ptr_cmp);
}

static GdkPixbuf *state_icon_for(App *a, Package *p) {
    if (p->is_broken) return a->icon_broken;
    if (p->is_upgradable) return a->icon_upgradable;
    if (p->is_installed) return a->icon_installed;
    return NULL;
}

static void package_colors(Package *p, const char **bg, const char **fg, gboolean *set) {
    *bg=NULL; *fg=NULL; *set=FALSE;
    if (p->is_broken) { *bg="#8f2424"; *fg="#ffffff"; *set=TRUE; }
    else if (p->is_upgradable) { *bg="#d97818"; *fg="#ffffff"; *set=TRUE; }
}

static gboolean package_matches_filter(App *a, Package *p) {
    switch(a->filter_mode){
        case F_INSTALLED:return p->is_installed;
        case F_UPGRADABLE:return p->is_upgradable;
        case F_NOT_INSTALLED:return !p->is_installed;
        case F_BROKEN:return p->is_broken;
        default:return TRUE;
    }
}

/* Search ranking keeps package names useful like Synaptic: exact name first,
 * then prefix/name matches, and only then description matches. One-character
 * searches are deliberately ignored because they match most large catalogs. */
static int package_search_rank(App *a, Package *p) {
    const char *q=a->search_folded ? a->search_folded : "";
    if(!*q || g_utf8_strlen(q,-1)<2 || !p->search_key) return 0;
    const char *nl=strchr(p->search_key,'\n');
    gsize name_len=nl?(gsize)(nl-p->search_key):strlen(p->search_key);
    gsize qlen=strlen(q);
    if(name_len==qlen && !memcmp(p->search_key,q,qlen)) return 1;
    if(name_len>=qlen && !memcmp(p->search_key,q,qlen)) return 2;
    if(g_strstr_len(p->search_key,(gssize)name_len,q)) return 3;
    if(nl && strstr(nl+1,q)) return 4;
    return 0;
}

static void append_package_row(App *a, Package *p) {
    GtkTreeIter it; const char *bg,*fg; gboolean colored;
    package_colors(p,&bg,&fg,&colored);
    const char *candidate=(a->catalog_loaded && p->candidate && *p->candidate)?p->candidate:"—";
    gtk_list_store_append(a->store,&it);
    gtk_list_store_set(a->store,&it,
        C_ACTION,action_text(p->action), C_STATE_ICON,state_icon_for(a,p), C_NAME,p->name,
        C_INSTALLED,p->installed, C_CANDIDATE,candidate, C_ARCH,p->arch,
        C_BG,bg, C_FG,fg, C_HAS_COLOR,colored,-1);
}

static void rebuild_model(App *a) {
    if (!a->sorted_packages) rebuild_sorted_packages(a);
    gtk_tree_view_set_model(GTK_TREE_VIEW(a->tree), NULL);
    gtk_list_store_clear(a->store);

    const char *q=a->search_folded ? a->search_folded : "";
    const gboolean ranked=*q && g_utf8_strlen(q,-1)>=2;
    if(!ranked){
        for(guint i=0;i<a->sorted_packages->len;i++){
            Package *p=g_ptr_array_index(a->sorted_packages,i);
            if(package_matches_filter(a,p)) append_package_row(a,p);
        }
    }else{
        GPtrArray *bucket[4]={g_ptr_array_new(),g_ptr_array_new(),g_ptr_array_new(),g_ptr_array_new()};
        guint name_matches=0;
        for(guint i=0;i<a->sorted_packages->len;i++){
            Package *p=g_ptr_array_index(a->sorted_packages,i);
            /* Search results span the complete catalog regardless of sidebar filter. */
            int rank=package_search_rank(a,p);
            if(rank>=1&&rank<=4){
                g_ptr_array_add(bucket[rank-1],p);
                if(rank<=3) name_matches++;
            }
        }
        for(int b=0;b<3;b++)
            for(guint i=0;i<bucket[b]->len;i++) append_package_row(a,g_ptr_array_index(bucket[b],i));
        /* Descriptions are useful as a fallback, but not when the name search
         * already produced a large useful result set. */
        if(name_matches<50)
            for(guint i=0;i<bucket[3]->len;i++) append_package_row(a,g_ptr_array_index(bucket[3],i));
        for(int b=0;b<4;b++) g_ptr_array_free(bucket[b],TRUE);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(a->tree), GTK_TREE_MODEL(a->store));
}

static guint marked_count(App *a) {
    guint n=0; GHashTableIter hi; gpointer k,v;g_hash_table_iter_init(&hi,a->packages);while(g_hash_table_iter_next(&hi,&k,&v))if(((Package*)v)->action!=ACT_NONE)n++;return n;
}

static void update_summary(App *a) {
    guint total=g_hash_table_size(a->packages),inst=0,up=0,broken=0;
    GHashTableIter hi;gpointer k,v;g_hash_table_iter_init(&hi,a->packages);
    while(g_hash_table_iter_next(&hi,&k,&v)){Package*p=v;if(p->is_installed)inst++;if(p->is_upgradable)up++;if(p->is_broken)broken++;}
    TrKey keys[5]={T_ALL,T_INSTALLED,T_UPGRADABLE,T_NOT_INSTALLED,T_BROKEN};
    if(!a->catalog_loaded){
        for(int i=0;i<5;i++) if(a->filter_buttons[i]){
            gchar *lbl=NULL;
            if(i==F_INSTALLED)lbl=g_strdup_printf("%s  (%u)",tr(keys[i]),inst);
            else if(i==F_BROKEN)lbl=g_strdup_printf("%s  (%u)",tr(keys[i]),broken);
            else lbl=g_strdup_printf("%s  (—)",tr(keys[i]));
            gtk_button_set_label(GTK_BUTTON(a->filter_buttons[i]),lbl);g_free(lbl);
        }
        char *text=g_strdup_printf("%s: %u   •   %s: %u",tr(T_INSTALLED),inst,tr(T_BROKEN),broken);
        gtk_label_set_text(GTK_LABEL(a->status_label),text);g_free(text);
        return;
    }
    guint counts[5]={total,inst,up,total-inst,broken};
    for(int i=0;i<5;i++) if(a->filter_buttons[i]){
        char *lbl=g_strdup_printf("%s  (%u)",tr(keys[i]),counts[i]);
        gtk_button_set_label(GTK_BUTTON(a->filter_buttons[i]),lbl);g_free(lbl);
    }
    char *text=g_strdup_printf("%s: %u   •   %s: %u   •   %s: %u   •   %s: %u",
        tr(T_ALL),total,tr(T_INSTALLED),inst,tr(T_UPGRADABLE),up,tr(T_BROKEN),broken);
    gtk_label_set_text(GTK_LABEL(a->status_label),text);g_free(text);
}

typedef struct {
    App *a;
    gboolean force;
    gboolean local_only;
    gboolean catalog_loaded;
    GHashTable *packages;
    char native_arch[64];
    gboolean from_cache;
    char *error;
} LoadJob;

static gboolean load_finish_idle(gpointer data) {
    LoadJob *j=data; App *a=j->a;
    if(a->load_thread){GThread*t=a->load_thread;a->load_thread=NULL;g_thread_join(t);}
    eapt_debug_log("DEB load_finish begin force=%d local_only=%d from_cache=%d catalog_loaded=%d new_packages=%u error=%s",
        j->force, j->local_only, j->from_cache, j->catalog_loaded,
        j->packages ? g_hash_table_size(j->packages) : 0, j->error ? j->error : "");
    GHashTable *old=a->packages; a->packages=j->packages; j->packages=NULL;
    if (old) g_hash_table_destroy(old);
    g_strlcpy(a->native_arch,j->native_arch,sizeof(a->native_arch));
    a->catalog_loaded=j->catalog_loaded;
    if(j->force && j->catalog_loaded && (!j->error || !*j->error))a->repo_refreshed=TRUE;
    if(j->force && j->catalog_loaded && (!j->error || !*j->error)){
        /* Refresh always lands on the complete freshly rebuilt catalog. */
        a->filter_mode=F_ALL;
        g_free(a->search_folded);
        a->search_folded=g_strdup("");
        if(a->search && *gtk_entry_get_text(GTK_ENTRY(a->search)))
            gtk_entry_set_text(GTK_ENTRY(a->search),"");
    }
    rebuild_sorted_packages(a);
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree)));
    rebuild_model(a); update_summary(a); refresh_details(a);
    if(a->repo_refreshed)gtk_widget_hide(a->repo_notice);
    else gtk_widget_show(a->repo_notice);
    if(j->error && *j->error)
        gtk_label_set_text(GTK_LABEL(a->status_label),j->error);
    a->loading=FALSE;
    gboolean controls_enabled=a->is_root&&!a->busy;
    gtk_widget_set_sensitive(a->btn_refresh,!a->busy);
    gtk_widget_set_sensitive(a->btn_mark_upgrades,controls_enabled);
    gtk_widget_set_sensitive(a->btn_apply,controls_enabled);
    gtk_widget_set_sensitive(a->btn_fix,controls_enabled);
    gtk_widget_set_sensitive(a->btn_clean,controls_enabled);

    /* Refresh owns the complete catalog rebuild.  Keep its progress window
     * alive while the worker parses/saves the refreshed catalog, then close it
     * automatically only when the whole Refresh operation has really finished. */
    if(a->close_progress_after_load&&a->progress_dialog){
        if(j->error&&*j->error){
            if(a->pulse_id){g_source_remove(a->pulse_id);a->pulse_id=0;}
            if(a->progress_label)gtk_label_set_text(GTK_LABEL(a->progress_label),tr(T_FAILED));
            progress_append(a,j->error);
            if(a->progress_close)gtk_widget_set_sensitive(a->progress_close,TRUE);
            if(a->progress_cancel)gtk_widget_set_sensitive(a->progress_cancel,FALSE);
            gtk_window_set_deletable(GTK_WINDOW(a->progress_dialog),TRUE);
        }else{
            GtkWidget *dlg=a->progress_dialog;
            if(a->pulse_id){g_source_remove(a->pulse_id);a->pulse_id=0;}
            a->close_progress_after_load=FALSE;
            gtk_window_set_modal(GTK_WINDOW(dlg),FALSE);
            gtk_widget_destroy(dlg);
            gtk_window_present(GTK_WINDOW(a->window));
            gtk_widget_grab_focus(a->search);
        }
    }
    if(!a->progress_dialog)a->close_progress_after_load=FALSE;
    eapt_debug_widget("DEB load_finish main-window", a->window);
    eapt_debug_schedule_toplevels("DEB load_finish +250ms", 250);
    gboolean run_queued_refresh=a->refresh_queued&&!a->busy;
    if(run_queued_refresh)a->refresh_queued=FALSE;
    g_free(j->error);g_free(j);
    if(run_queued_refresh)reload_data_async(a,TRUE);
    return G_SOURCE_REMOVE;
}

static gpointer load_worker(gpointer data) {
    LoadJob *j=data; App tmp={0};
    tmp.backend=j->a->backend;
    tmp.packages=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,package_free);
    load_native_arch(&tmp);
    guint repo_files=0, status_entries=0;
    gboolean loaded=FALSE;

    if(j->local_only){
        /* First paint: local truth only.  Never touch the 70k repository catalog
         * and never access the network. */
        status_entries=parse_status_fast(&tmp);
        finalize_package_states(&tmp);
        j->catalog_loaded=FALSE;
    }else if(!j->force){
        /* On-demand catalog: use only the persistent cache. If it is absent or
         * stale, keep the live installed list and ask the user to Refresh. */
        loaded=load_package_cache(&tmp);
        if(loaded){
            clear_local_package_state(&tmp);
            status_entries=parse_status_fast(&tmp);
            finalize_package_states(&tmp);
            j->catalog_loaded=TRUE;
        }else{
            status_entries=parse_status_fast(&tmp);
            finalize_package_states(&tmp);
            j->catalog_loaded=FALSE;
            j->error=g_strdup("No cached package catalog is available. Use Refresh to update repositories and build it.");
        }
    }else{
        /* Explicit Refresh owns the expensive repository-catalog rebuild. */
        repo_files=parse_available_fast(&tmp);
        status_entries=parse_status_fast(&tmp);
        overlay_apt_policy_candidates(&tmp);
        finalize_package_states(&tmp);
        if(g_hash_table_size(tmp.packages)>0) save_package_cache(&tmp);
        if(repo_files>0) j->catalog_loaded=TRUE;
        else if(status_entries==0)
            j->error=g_strdup_printf("%s: no package indexes or installed-package database were found.", backend_name(&tmp));
    }

    j->from_cache=loaded; j->packages=tmp.packages; tmp.packages=NULL;
    g_strlcpy(j->native_arch,tmp.native_arch,sizeof(j->native_arch));
    g_idle_add(load_finish_idle,j); return NULL;
}

static void start_load_async(App *a, gboolean force, gboolean local_only) {
    eapt_debug_log("DEB load requested force=%d local_only=%d loading=%d busy=%d packages=%u",
        force, local_only, a->loading, a->busy, a->packages ? g_hash_table_size(a->packages) : 0);
    if(a->loading) {
        if(force){a->refresh_queued=TRUE;eapt_debug_log("DEB refresh queued while loader is active");}
        else eapt_debug_log("DEB load skipped: already loading");
        return;
    }
    a->loading=TRUE;
    gtk_widget_set_sensitive(a->btn_refresh,FALSE);
    gtk_widget_set_sensitive(a->btn_mark_upgrades,FALSE);
    gtk_widget_set_sensitive(a->btn_apply,FALSE);
    gtk_widget_set_sensitive(a->btn_fix,FALSE);
    gtk_widget_set_sensitive(a->btn_clean,FALSE);
    if(g_hash_table_size(a->packages)==0) gtk_label_set_text(GTK_LABEL(a->status_label),tr(T_RUNNING));
    LoadJob*j=g_new0(LoadJob,1);j->a=a;j->force=force;j->local_only=local_only;
    if(a->load_thread){GThread*t=a->load_thread;a->load_thread=NULL;g_thread_join(t);}
    a->load_thread=g_thread_new(local_only?"edpkg-gui-local-loader":"edpkg-gui-catalog-loader",load_worker,j);
}

static void reload_data_async(App *a, gboolean force) { start_load_async(a,force,FALSE); }
static void load_local_async(App *a) { start_load_async(a,FALSE,TRUE); }

static GPtrArray *selected_packages(App *a) {
    GtkTreeSelection *sel=gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree));
    GtkTreeModel *model=NULL;
    GList *rows=gtk_tree_selection_get_selected_rows(sel,&model);
    GPtrArray *items=g_ptr_array_new();
    for(GList *l=rows;l;l=l->next){
        GtkTreeIter it;
        if(gtk_tree_model_get_iter(model,&it,(GtkTreePath*)l->data)){
            char *name=NULL;
            gtk_tree_model_get(model,&it,C_NAME,&name,-1);
            Package *pkg=g_hash_table_lookup(a->packages,name);
            if(pkg)g_ptr_array_add(items,pkg);
            g_free(name);
        }
        gtk_tree_path_free((GtkTreePath*)l->data);
    }
    g_list_free(rows);
    return items;
}

static void refresh_details(App *a) {
    GPtrArray *items=selected_packages(a);
    if(items->len==0){
        gtk_label_set_text(GTK_LABEL(a->detail_name),tr(T_NO_SELECTION));
        gtk_label_set_text(GTK_LABEL(a->detail_desc),"");gtk_label_set_text(GTK_LABEL(a->detail_versions),"");
        gtk_widget_set_sensitive(a->btn_install,FALSE);gtk_widget_set_sensitive(a->btn_remove,FALSE);gtk_widget_set_sensitive(a->btn_reinstall,FALSE);gtk_widget_set_sensitive(a->btn_clear,FALSE);
        g_ptr_array_free(items,TRUE);return;
    }
    if(items->len>1){
        gboolean any_inst=FALSE,any_not=FALSE,any_marked=FALSE;
        for(guint i=0;i<items->len;i++){Package *p=g_ptr_array_index(items,i);if(p->is_installed)any_inst=TRUE;else any_not=TRUE;if(p->action!=ACT_NONE)any_marked=TRUE;}
        const char *fmt=tr(T_MULTI_SELECTED);
        const char *mark=strstr(fmt,"%u");
        char num[32];g_snprintf(num,sizeof(num),"%u",items->len);
        char *cnt=NULL;
        if(mark){
            char *left=g_strndup(fmt,(gsize)(mark-fmt));
            cnt=g_strconcat(left,num,mark+2,NULL);
            g_free(left);
        }else cnt=g_strdup_printf("%s %s",fmt,num);
        char *nm=g_strdup_printf("<b>%s</b>",cnt);g_free(cnt);gtk_label_set_markup(GTK_LABEL(a->detail_name),nm);g_free(nm);
        gtk_label_set_text(GTK_LABEL(a->detail_desc),tr(T_MULTI_HELP));
        gtk_label_set_text(GTK_LABEL(a->detail_versions),"");
        gtk_widget_set_sensitive(a->btn_install,any_not);gtk_widget_set_sensitive(a->btn_remove,any_inst);gtk_widget_set_sensitive(a->btn_reinstall,any_inst);gtk_widget_set_sensitive(a->btn_clear,any_marked);
        g_ptr_array_free(items,TRUE);return;
    }
    Package *p=g_ptr_array_index(items,0);
    char *nm=g_markup_printf_escaped("<b>%s</b>",p->name);gtk_label_set_markup(GTK_LABEL(a->detail_name),nm);g_free(nm);
    gtk_label_set_text(GTK_LABEL(a->detail_desc),*p->description?p->description:tr(T_DESCRIPTION));
    const char *candidate=(a->catalog_loaded&&p->candidate&&*p->candidate)?p->candidate:"—";char *vs=g_strdup_printf("%s: %s    %s: %s    %s: %s",tr(T_INSTALLED_VER),*p->installed?p->installed:"—",tr(T_CANDIDATE),candidate,tr(T_ARCH),*p->arch?p->arch:a->native_arch);gtk_label_set_text(GTK_LABEL(a->detail_versions),vs);g_free(vs);
    gboolean blocked=package_is_kernel_blocked(p->name);
    gtk_widget_set_sensitive(a->btn_install,!blocked&&!p->is_installed);gtk_widget_set_sensitive(a->btn_remove,!blocked&&p->is_installed);gtk_widget_set_sensitive(a->btn_reinstall,!blocked&&p->is_installed);gtk_widget_set_sensitive(a->btn_clear,!blocked&&p->action!=ACT_NONE);
    g_ptr_array_free(items,TRUE);
}

static void set_selected_action(App *a, ActionMode act) {
    GtkTreeSelection *sel=gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree));
    GtkTreeModel *model=NULL; GList *rows=gtk_tree_selection_get_selected_rows(sel,&model);
    for(GList*l=rows;l;l=l->next){
        GtkTreeIter it;
        if(gtk_tree_model_get_iter(model,&it,l->data)){
            char*name=NULL;gtk_tree_model_get(model,&it,C_NAME,&name,-1);
            Package*p=g_hash_table_lookup(a->packages,name);
            if(p && !package_is_kernel_blocked(p->name)){
                gboolean compatible=TRUE;
                if(act==ACT_INSTALL&&p->is_installed)compatible=FALSE;
                if((act==ACT_REMOVE||act==ACT_REINSTALL)&&!p->is_installed)compatible=FALSE;
                if(compatible){p->action=act;gtk_list_store_set(a->store,&it,C_ACTION,action_text(p->action),-1);}
            }
            g_free(name);
        }
        gtk_tree_path_free(l->data);
    }
    g_list_free(rows); update_summary(a); refresh_details(a);
}

/* -------------------------------------------------------------------------
 * Progress dialog + PTY input. Adapted from the DEB operation model used by
 * Essora Store: package-manager output is attached to a PTY and the text area
 * can feed answers back to maintainer/debconf prompts.
 * ------------------------------------------------------------------------- */

static gboolean progress_was_at_bottom(App *a) {
    if (!a->progress_scroller) return TRUE;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(a->progress_scroller));
    if (!adj) return TRUE;
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >= gtk_adjustment_get_upper(adj) - 8.0;
}

static void progress_trim(App *a) {
    if (!a->progress_buffer) return;
    gint chars = gtk_text_buffer_get_char_count(a->progress_buffer);
    if (chars <= 500000) return;
    GtkTextIter start, cut;
    gtk_text_buffer_get_start_iter(a->progress_buffer, &start);
    gtk_text_buffer_get_iter_at_offset(a->progress_buffer, &cut, chars - 400000);
    gtk_text_buffer_delete(a->progress_buffer, &start, &cut);
}

static void progress_append(App *a,const char *text){
    if(!a->progress_buffer || !text || !*text)return;
    gboolean at_bottom = progress_was_at_bottom(a);
    GtkTextIter e;gtk_text_buffer_get_end_iter(a->progress_buffer,&e);
    gtk_text_buffer_insert(a->progress_buffer,&e,text,-1);
    if(text[strlen(text)-1] != '\n') gtk_text_buffer_insert(a->progress_buffer,&e,"\n",-1);
    progress_trim(a);
    if(at_bottom && a->progress_text){
        GtkTextMark*m=gtk_text_buffer_get_insert(a->progress_buffer);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(a->progress_text),m,0.0,FALSE,0.0,1.0);
    }
}

static gboolean progress_flush_cb(gpointer d){
    App *a=d;
    gchar *chunk=NULL;
    g_mutex_lock(&a->progress_lock);
    if(a->progress_pending && a->progress_pending->len){
        chunk=g_strdup(a->progress_pending->str);
        g_string_truncate(a->progress_pending,0);
    }
    g_mutex_unlock(&a->progress_lock);
    if(chunk){progress_append(a,chunk);g_free(chunk);}
    if(!a->busy){a->progress_flush_id=0;return G_SOURCE_REMOVE;}
    return G_SOURCE_CONTINUE;
}

static void progress_append_thread(App*a,const char*line){
    if(!line || !*line)return;
    g_mutex_lock(&a->progress_lock);
    if(!a->progress_pending)a->progress_pending=g_string_new("");
    g_string_append(a->progress_pending,line);
    if(line[strlen(line)-1] != '\n')g_string_append_c(a->progress_pending,'\n');
    g_mutex_unlock(&a->progress_lock);
}

static gboolean pulse_cb(gpointer d){
    App*a=d;
    if(!a->busy&&!a->loading&&!a->close_progress_after_load){a->pulse_id=0;return G_SOURCE_REMOVE;}
    if(a->progress_bar)gtk_progress_bar_pulse(GTK_PROGRESS_BAR(a->progress_bar));
    return G_SOURCE_CONTINUE;
}

static void pty_write_input(App *a){
    if(!a||a->pty_master<0||!a->pty_input||!a->pty_input->len)return;
    gsize off=0;
    while(off<a->pty_input->len){
        ssize_t n=write(a->pty_master,a->pty_input->str+off,a->pty_input->len-off);
        if(n>0){off+=(gsize)n;continue;}
        if(n<0&&errno==EINTR)continue;
        break;
    }
}

static gboolean progress_key(GtkWidget*w,GdkEventKey*e,gpointer d){(void)w;App*a=d;if(a->pty_master<0)return FALSE;if(e->keyval==GDK_KEY_Return||e->keyval==GDK_KEY_KP_Enter){g_string_append_c(a->pty_input,'\n');pty_write_input(a);g_string_truncate(a->pty_input,0);return TRUE;}if(e->keyval==GDK_KEY_BackSpace){if(a->pty_input->len)g_string_truncate(a->pty_input,a->pty_input->len-1);return TRUE;}guint32 uc=gdk_keyval_to_unicode(e->keyval);if(uc&&g_unichar_isprint(uc)){char b[8]={0};gint n=g_unichar_to_utf8(uc,b);g_string_append_len(a->pty_input,b,n);return TRUE;}return FALSE;}

static void progress_dialog_destroyed(GtkWidget*w,gpointer d){
    App*a=d;
    eapt_debug_log("DEB progress destroy callback dialog=%p current=%p busy=%d pty_master=%d",
        (void*)w, (void*)a->progress_dialog, a->busy, a->pty_master);
    eapt_debug_widget("DEB progress destroy callback", w);
    if(a->progress_dialog==w){
        a->progress_dialog=NULL;
        a->progress_bar=NULL;
        a->progress_label=NULL;
        a->progress_text=NULL;
        a->progress_buffer=NULL;
        a->progress_close=NULL;
        a->progress_cancel=NULL;
        a->progress_scroller=NULL;
        a->close_progress_after_load=FALSE;
    }
    eapt_debug_schedule_toplevels("DEB after progress destroy +100ms",100);
    eapt_debug_schedule_toplevels("DEB after progress destroy +750ms",750);
}

static void progress_cancel_clicked(GtkButton*b,gpointer d){
    (void)b;App*a=d;
    if(!a->busy)return;
    if(g_atomic_int_get(&a->cancel_locked)){
        progress_append(a,"Cancellation is disabled while the package database is being modified.");
        return;
    }
    if(g_atomic_int_exchange(&a->cancel_requested,1))return;
    gint pid=g_atomic_int_get(&a->active_child_pid);
    progress_append(a,"Cancellation requested…");
    if(a->progress_cancel)gtk_widget_set_sensitive(a->progress_cancel,FALSE);
    if(pid>0){
        if(kill(-pid,SIGINT)!=0)(void)kill(pid,SIGINT);
    }
}

static gboolean main_delete_event(GtkWidget*w,GdkEvent*e,gpointer d){
    (void)w;(void)e;App*a=d;
    /* A running transaction owns App state and must finish/cancel first.
     * Catalog loading is safe to close: main() joins load_thread before cleanup. */
    return a->busy;
}

static void progress_show(App*a,const char*title){
    if(a->progress_dialog)gtk_widget_destroy(a->progress_dialog);
    a->pty_master=-1;if(a->pty_input)g_string_free(a->pty_input,TRUE);a->pty_input=g_string_new("");
    /* Do NOT make the long-running progress window WM-modal.  Fluxbox keeps
     * modal/transient focus state, and an auto-destroyed modal progress
     * window can leave the parent unable to regain focus after PyMenu takes
     * it.  The operation buttons are already disabled while busy, so a
     * transient non-modal dialog is sufficient and avoids the stale modal
     * relationship entirely. */
    a->progress_dialog=gtk_dialog_new_with_buttons(title,GTK_WINDOW(a->window),GTK_DIALOG_DESTROY_WITH_PARENT,NULL,NULL);
    gtk_window_set_modal(GTK_WINDOW(a->progress_dialog),FALSE);
    eapt_debug_log("DEB progress_show created title=%s dialog=%p parent=%p modal=%d",
        title?title:"", (void*)a->progress_dialog, (void*)a->window, gtk_window_get_modal(GTK_WINDOW(a->progress_dialog)));
    eapt_debug_watch_window(a->progress_dialog,"DEB-PROGRESS");
    g_signal_connect(a->progress_dialog,"destroy",G_CALLBACK(progress_dialog_destroyed),a);
    gtk_window_set_default_size(GTK_WINDOW(a->progress_dialog),700,430);
    gtk_window_set_deletable(GTK_WINDOW(a->progress_dialog),FALSE);
    GtkWidget*area=gtk_dialog_get_content_area(GTK_DIALOG(a->progress_dialog));GtkWidget*v=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_container_set_border_width(GTK_CONTAINER(v),12);gtk_box_pack_start(GTK_BOX(area),v,TRUE,TRUE,0);
    a->progress_label=gtk_label_new(tr(T_RUNNING));gtk_label_set_xalign(GTK_LABEL(a->progress_label),0);gtk_box_pack_start(GTK_BOX(v),a->progress_label,FALSE,FALSE,0);
    a->progress_bar=gtk_progress_bar_new();gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(a->progress_bar),FALSE);gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(a->progress_bar),0.05);gtk_box_pack_start(GTK_BOX(v),a->progress_bar,FALSE,FALSE,0);
    GtkWidget*sc=gtk_scrolled_window_new(NULL,NULL);a->progress_scroller=sc;gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);gtk_box_pack_start(GTK_BOX(v),sc,TRUE,TRUE,0);
    a->progress_text=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(a->progress_text),FALSE);gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(a->progress_text),TRUE);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->progress_text),GTK_WRAP_WORD_CHAR);gtk_text_view_set_monospace(GTK_TEXT_VIEW(a->progress_text),TRUE);gtk_container_add(GTK_CONTAINER(sc),a->progress_text);a->progress_buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->progress_text));g_signal_connect(a->progress_text,"key-press-event",G_CALLBACK(progress_key),a);
    a->progress_cancel=gtk_dialog_add_button(GTK_DIALOG(a->progress_dialog),tr(T_CANCEL),GTK_RESPONSE_CANCEL);
    a->progress_close=gtk_dialog_add_button(GTK_DIALOG(a->progress_dialog),tr(T_CLOSE),GTK_RESPONSE_CLOSE);
    gtk_widget_set_sensitive(a->progress_close,FALSE);
    g_signal_connect(a->progress_cancel,"clicked",G_CALLBACK(progress_cancel_clicked),a);
    g_signal_connect_swapped(a->progress_close,"clicked",G_CALLBACK(gtk_widget_destroy),a->progress_dialog);
    gtk_widget_show_all(a->progress_dialog);
    eapt_debug_widget("DEB progress shown",a->progress_dialog);
    eapt_debug_toplevels("DEB progress shown");
    a->pulse_id=g_timeout_add(100,pulse_cb,a);
    if(a->progress_flush_id)g_source_remove(a->progress_flush_id);
    a->progress_flush_id=g_timeout_add(100,progress_flush_cb,a);
}


static GHashTable *capture_local_state_snapshot(App *a) {
    App tmp={0};
    tmp.backend=a->backend;
    tmp.packages=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,package_free);
    load_native_arch(&tmp);
    parse_status_fast(&tmp);
    return tmp.packages;
}

static void overlay_local_state(App *a,GHashTable *local) {
    if(!a||!local)return;
    clear_local_package_state(a);
    gboolean added=FALSE;
    GHashTableIter it;gpointer key,value;
    g_hash_table_iter_init(&it,local);
    while(g_hash_table_iter_next(&it,&key,&value)){
        Package *src=value;
        Package *dst=g_hash_table_lookup(a->packages,src->name);
        if(!dst){
            dst=package_new(src->name);
            g_hash_table_insert(a->packages,g_strdup(src->name),dst);
            added=TRUE;
        }
        set_string(&dst->installed,src->installed);
        set_string(&dst->status,src->status);
        if(src->arch&&*src->arch)set_string(&dst->arch,src->arch);
        if((!dst->description||!*dst->description)&&src->description&&*src->description)
            set_string(&dst->description,src->description);
        dst->is_installed=src->is_installed;
        dst->is_broken=src->is_broken;
        dst->held=src->held;
        dst->action=ACT_NONE;
        package_refresh_search_key(dst);
    }
    recompute_upgrade_flags(a);
    if(added||!a->sorted_packages)rebuild_sorted_packages(a);
    rebuild_model(a);
    update_summary(a);
    refresh_details(a);
}

static void reload_data_async(App *a, gboolean force);
typedef struct{App*a;gboolean ok;gboolean cancelled;gboolean close_on_success;gboolean refresh_after;char*msg;GHashTable*local_state;} Finish;
static gboolean idle_finish(gpointer d){
    Finish*f=d;App*a=f->a;
    if(a->worker_thread){GThread*t=a->worker_thread;a->worker_thread=NULL;g_thread_join(t);}
    eapt_debug_log("DEB idle_finish begin ok=%d close_on_success=%d refresh_after=%d progress=%p main=%p busy=%d loading=%d",
        f->ok,f->close_on_success,f->refresh_after,(void*)a->progress_dialog,(void*)a->window,a->busy,a->loading);
    eapt_debug_toplevels("DEB idle_finish begin");
    a->busy=FALSE;a->pty_master=-1;
    /* A cancelled package transaction may already have changed local state.
     * Re-read/overlay it on both success and cancellation. */
    if(f->local_state)overlay_local_state(a,f->local_state);
    if(f->local_state){g_hash_table_destroy(f->local_state);f->local_state=NULL;}

    if(a->progress_flush_id){g_source_remove(a->progress_flush_id);a->progress_flush_id=0;}
    (void)progress_flush_cb(a);
    if(a->progress_bar)gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(a->progress_bar),FALSE);
    if(f->msg&&*f->msg)progress_append(a,f->msg);

    if(f->ok&&f->refresh_after){
        /* Updating indexes is only the first half of Refresh.  Keep the
         * activity window open while the refreshed package catalog is parsed
         * and cached; load_finish_idle closes it automatically afterwards. */
        a->close_progress_after_load=f->close_on_success;
        if(a->progress_cancel)gtk_widget_set_sensitive(a->progress_cancel,FALSE);
        if(a->progress_label)gtk_label_set_text(GTK_LABEL(a->progress_label),tr(T_REFRESHING_REPOS));
        if(!a->pulse_id)a->pulse_id=g_timeout_add(100,pulse_cb,a);
        reload_data_async(a,TRUE);
    }else{
        if(a->pulse_id){g_source_remove(a->pulse_id);a->pulse_id=0;}
        if(!f->ok&&a->progress_bar)gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress_bar),0.0);
        if(a->progress_label)gtk_label_set_text(GTK_LABEL(a->progress_label),
            f->cancelled?tr(T_CANCELLED):(f->ok?tr(T_DONE):tr(T_FAILED)));
        if(a->progress_close)gtk_widget_set_sensitive(a->progress_close,TRUE);
        if(a->progress_cancel)gtk_widget_set_sensitive(a->progress_cancel,FALSE);
        if(a->progress_dialog)gtk_window_set_deletable(GTK_WINDOW(a->progress_dialog),TRUE);
        if(f->ok&&f->close_on_success&&a->progress_dialog){
            GtkWidget*dlg=a->progress_dialog;
            gtk_window_set_modal(GTK_WINDOW(dlg),FALSE);
            gtk_widget_destroy(dlg);
            gtk_window_present(GTK_WINDOW(a->window));
            gtk_widget_grab_focus(a->search);
        }
    }

    if(!f->ok && f->refresh_after)gtk_label_set_text(GTK_LABEL(a->status_label),
        f->cancelled?tr(T_CANCELLED):tr(T_FAILED));
    gtk_widget_set_sensitive(a->btn_refresh,!a->busy&&!a->loading);
    gtk_widget_set_sensitive(a->btn_mark_upgrades,a->is_root&&!a->loading);
    gtk_widget_set_sensitive(a->btn_apply,a->is_root&&!a->loading);
    gtk_widget_set_sensitive(a->btn_fix,a->is_root&&!a->loading);
    gtk_widget_set_sensitive(a->btn_clean,a->is_root&&!a->loading);
    g_free(f->msg);g_free(f);return G_SOURCE_REMOVE;
}
static void finish_thread(App*a,gboolean ok,gboolean cancelled,gboolean close_on_success,gboolean refresh_after,const char*msg,GHashTable*local_state){Finish*f=g_new0(Finish,1);f->a=a;f->ok=ok;f->cancelled=cancelled;f->close_on_success=close_on_success;f->refresh_after=refresh_after;f->msg=g_strdup(msg?msg:"");f->local_state=local_state;g_idle_add(idle_finish,f);}

static gchar *clean_terminal_text(const char *src) {
    if(!src)return g_strdup("");
    static GRegex *ansi=NULL;
    if(!ansi)ansi=g_regex_new("\\x1B\\[[0-?]*[ -/]*[@-~]",0,0,NULL);
    return g_regex_replace(ansi,src,-1,0,"",0,NULL);
}

static gboolean cancel_lock_idle(gpointer d){
    App *a=d;
    if(a->progress_cancel)gtk_widget_set_sensitive(a->progress_cancel,FALSE);
    return G_SOURCE_REMOVE;
}

static gboolean cancel_unlock_idle(gpointer d){
    App *a=d;
    if(a->busy&&a->progress_cancel&&!g_atomic_int_get(&a->cancel_requested))
        gtk_widget_set_sensitive(a->progress_cancel,TRUE);
    return G_SOURCE_REMOVE;
}

/* Simulations and non-mutating package-manager phases are safe to cancel. */
static void cancel_phase_safe(App *a){
    if(g_atomic_int_get(&a->cancel_requested))return;
    g_atomic_int_set(&a->cancel_download_mode,0);
    g_atomic_int_set(&a->cancel_download_seen,0);
    g_atomic_int_set(&a->cancel_locked,0);
    g_idle_add(cancel_unlock_idle,a);
}

/* Real install/remove/reinstall/fix/autoremove commands are fail-closed.
 * Cancellation starts disabled.  We temporarily re-enable it only while
 * observing backend download lines that are known not to modify dpkg/EDPKG
 * state.  Unknown locales therefore stay on the safe side automatically. */
static void cancel_phase_package_command(App *a){
    if(g_atomic_int_get(&a->cancel_requested))return;
    g_atomic_int_set(&a->cancel_download_mode,1);
    g_atomic_int_set(&a->cancel_download_seen,0);
    if(!g_atomic_int_exchange(&a->cancel_locked,1))
        g_idle_add(cancel_lock_idle,a);
}

/* APT localizes words such as Get/Hit/Fetched, but keeps the indexed fetch
 * shape: <token>:<number> <URI>.  Recognize that structure instead of a
 * language-specific token.  Summary lines (Fetched/Descargados/...) do not
 * match, so cancellation closes conservatively as soon as download output
 * ends. */
static gboolean line_is_safe_download(const char *line){
    if(!line||!*line)return FALSE;
    const char *colon=strchr(line,':');
    if(!colon)return FALSE;
    gsize prefix_len=(gsize)(colon-line);
    if(prefix_len<1||prefix_len>16)return FALSE;
    for(const char *p=line;p<colon;p++){
        if(g_ascii_isspace((guchar)*p)||g_ascii_iscntrl((guchar)*p))return FALSE;
    }
    const char *p=colon+1;
    if(!g_ascii_isdigit((guchar)*p))return FALSE;
    while(g_ascii_isdigit((guchar)*p))p++;
    if(*p!=' '&&*p!='\t')return FALSE;
    while(*p==' '||*p=='\t')p++;
    return strstr(p,"://")!=NULL||g_str_has_prefix(p,"file:/")||
           g_str_has_prefix(p,"copy:/")||g_str_has_prefix(p,"cdrom:");
}

static gboolean line_enters_package_mutation(const char *line){
    if(!line||!*line)return FALSE;
    /* English markers remain useful as an early/explicit lock, but safety no
     * longer depends on this translation list: real package commands begin
     * locked and only known download lines may open a cancellation window. */
    static const char *markers[]={
        "Unpacking ","Preparing to unpack ","Setting up ","Removing ","Purging ",
        "Desempaquetando ","Preparando para desempaquetar ","Configurando ",
        "S'està desempaquetant ","Entpacken von ","Dépaquetage de ","Estrazione di ",
        "Desempacotando ","Распаковка ","正在解包 ","جارٍ فك ",
        " kicsomagolása"," を展開しています",NULL
    };
    for(guint i=0;markers[i];i++)if(strstr(line,markers[i]))return TRUE;
    return FALSE;
}

static void process_pty_segment(App *a,const char *segment,gboolean carriage_only){
    if(!segment)return;
    gchar *clean=clean_terminal_text(segment);
    gchar *line=g_strstrip(clean);
    if(*line && !carriage_only){
        if(g_atomic_int_get(&a->cancel_download_mode)==1){
            if(line_is_safe_download(line)){
                g_atomic_int_set(&a->cancel_download_seen,1);
                if(g_atomic_int_exchange(&a->cancel_locked,0))
                    g_idle_add(cancel_unlock_idle,a);
            }else if(g_atomic_int_get(&a->cancel_download_seen)){
                /* Once download output ends, never reopen cancellation for
                 * this real transaction, regardless of locale. */
                g_atomic_int_set(&a->cancel_download_mode,2);
                if(!g_atomic_int_exchange(&a->cancel_locked,1))
                    g_idle_add(cancel_lock_idle,a);
            }else if(line_enters_package_mutation(line)){
                g_atomic_int_set(&a->cancel_download_mode,2);
                if(!g_atomic_int_exchange(&a->cancel_locked,1))
                    g_idle_add(cancel_lock_idle,a);
            }
        }else if(line_enters_package_mutation(line)&&
                 !g_atomic_int_exchange(&a->cancel_locked,1)){
            g_idle_add(cancel_lock_idle,a);
        }
        progress_append_thread(a,line);
    }
    g_free(clean);
}

static void feed_pty_bytes(App *a,GString *pending,const char *buf,gsize len){
    g_string_append_len(pending,buf,len);
    for(;;){
        gchar *cr=strchr(pending->str,'\r');
        gchar *nl=strchr(pending->str,'\n');
        gchar *cut=NULL;
        if(cr&&nl)cut=cr<nl?cr:nl;else cut=cr?cr:nl;
        if(!cut)break;
        gsize n=(gsize)(cut-pending->str);
        gboolean carriage_only=(*cut=='\r');
        gsize consume=1;
        if(carriage_only&&n+1<pending->len&&pending->str[n+1]=='\n'){carriage_only=FALSE;consume=2;}
        gchar *line=g_strndup(pending->str,n);
        process_pty_segment(a,line,carriage_only);
        g_free(line);
        g_string_erase(pending,0,n+consume);
    }
}

static int run_pty_exec(App*a,const char*exec_path,char*const argv[]) {
    if(g_atomic_int_get(&a->cancel_requested))return 130;
    int master=-1;pid_t pid=forkpty(&master,NULL,NULL,NULL);if(pid<0)return -1;
    if(pid==0){execv(exec_path,argv);_exit(127);}
    a->pty_master=master;
    g_atomic_int_set(&a->active_child_pid,(gint)pid);
    GString *pending=g_string_new("");
    gboolean child_done=FALSE, eof=FALSE,term_sent=FALSE,kill_sent=FALSE;
    gint child_status=0;
    gint64 child_done_since=0,cancel_since=0;
    gint64 last_data=g_get_monotonic_time();
    while(!eof){
        fd_set r;FD_ZERO(&r);FD_SET(master,&r);struct timeval tv={0,100000};
        int sel=select(master+1,&r,NULL,NULL,&tv);
        if(sel<0&&errno==EINTR)continue;
        if(sel>0&&FD_ISSET(master,&r)){
            char b[4096];ssize_t n=read(master,b,sizeof(b));
            if(n>0){feed_pty_bytes(a,pending,b,(gsize)n);last_data=g_get_monotonic_time();}
            else if(n==0||(n<0&&errno==EIO))eof=TRUE;
            else if(n<0&&errno!=EINTR&&errno!=EAGAIN)eof=TRUE;
        }
        if(!child_done){
            pid_t q=waitpid(pid,&child_status,WNOHANG);
            if(q==pid){child_done=TRUE;child_done_since=g_get_monotonic_time();}
        }
        if(g_atomic_int_get(&a->cancel_requested)&&!child_done&&!g_atomic_int_get(&a->cancel_locked)){
            gint64 now=g_get_monotonic_time();
            if(!cancel_since){cancel_since=now;if(kill(-pid,SIGINT)!=0)(void)kill(pid,SIGINT);}
            else if(!term_sent&&now-cancel_since>3000000){term_sent=TRUE;if(kill(-pid,SIGTERM)!=0)(void)kill(pid,SIGTERM);}
            else if(!kill_sent&&now-cancel_since>6000000){kill_sent=TRUE;if(kill(-pid,SIGKILL)!=0)(void)kill(pid,SIGKILL);}
        }
        if(pending->len && g_get_monotonic_time()-last_data>250000){
            /* Prompts frequently do not end in NL.  Show them rather than
             * leaving dpkg/debconf apparently frozen. */
            process_pty_segment(a,pending->str,FALSE);
            g_string_truncate(pending,0);
            last_data=g_get_monotonic_time();
        }
        if(child_done&&sel==0&&child_done_since>0&&g_get_monotonic_time()-child_done_since>500000){
            /* The slave is gone and no more bytes arrived for 500 ms. */
            eof=TRUE;
        }
    }
    if(pending->len)process_pty_segment(a,pending->str,FALSE);
    g_string_free(pending,TRUE);
    a->pty_master=-1;
    g_atomic_int_set(&a->active_child_pid,0);
    close(master);
    if(!child_done)waitpid(pid,&child_status,0);
    if(g_atomic_int_get(&a->cancel_requested))return 130;
    if(WIFEXITED(child_status))return WEXITSTATUS(child_status);
    if(WIFSIGNALED(child_status))return 128+WTERMSIG(child_status);
    return 1;
}

static int run_pty(App*a,char*const argv[]){return run_pty_exec(a,argv[0],argv);}

static gboolean check_root_or_warn(App*a){if(a->is_root)return TRUE;GtkWidget*d=gtk_message_dialog_new(GTK_WINDOW(a->window),GTK_DIALOG_MODAL,GTK_MESSAGE_WARNING,GTK_BUTTONS_CLOSE,"%s",tr(T_ROOT_REQUIRED));gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),"%s",tr(T_ROOT_REQUIRED_TEXT));gtk_dialog_run(GTK_DIALOG(d));gtk_widget_destroy(d);return FALSE;}

static void start_busy(App*a,const char*title){eapt_debug_log("DEB start_busy title=%s main=%p progress_before=%p",title?title:"",(void*)a->window,(void*)a->progress_dialog);a->busy=TRUE;gtk_widget_set_sensitive(a->btn_refresh,FALSE);gtk_widget_set_sensitive(a->btn_mark_upgrades,FALSE);gtk_widget_set_sensitive(a->btn_apply,FALSE);gtk_widget_set_sensitive(a->btn_fix,FALSE);gtk_widget_set_sensitive(a->btn_clean,FALSE);progress_show(a,title);}

typedef enum{JOB_UPDATE,JOB_APPLY,JOB_FIX,JOB_CLEAN,JOB_EDIT_SOURCES} JobType;
typedef struct{App*a;JobType type;gboolean update_first;gboolean autoremove;GPtrArray*install;GPtrArray*remove;GPtrArray*reinstall;} Job;
static void job_free(Job*j){if(!j)return;if(j->install)g_ptr_array_free(j->install,TRUE);if(j->remove)g_ptr_array_free(j->remove,TRUE);if(j->reinstall)g_ptr_array_free(j->reinstall,TRUE);g_free(j);}

/* Dependency-guard result codes.  A parser that does not understand the
 * simulated plan must never silently declare it safe. */
enum {
    SIM_CHECK_SAFE=0,
    SIM_CHECK_KERNEL=1,
    SIM_CHECK_FAILED=-1,
    SIM_CHECK_UNSUPPORTED=-2,
    SIM_CHECK_UNRECOGNIZED=-3,
    SIM_CHECK_CANCELLED=-4
};

static int simulation_output_kernel_check(const char *out,gchar **blocked_name,guint *recognized_lines){
    if(blocked_name)*blocked_name=NULL;
    if(recognized_lines)*recognized_lines=0;
    gboolean blocked=FALSE;
    guint recognized=0;
    gchar **lines=g_strsplit(out?out:"","\n",-1);
    for(guint i=0;lines[i];i++){
        gchar *line=g_strstrip(lines[i]);
        const char *start=NULL;
        if(g_str_has_prefix(line,"Inst ")){start=line+5;recognized++;}
        else if(g_str_has_prefix(line,"Remv ")){start=line+5;recognized++;}
        else if(g_str_has_prefix(line,"Fix ")){start=line+4;recognized++;}
        else if(g_str_has_prefix(line,"Download ")||g_str_has_prefix(line,"MarkManual ")||
                g_str_has_prefix(line,"Clean ")||g_str_has_prefix(line,"edpkg: transaction plan:")||
                g_str_has_prefix(line,"edpkg: 0 packages to autoremove")||
                g_str_has_prefix(line,"edpkg: fix-broken:")||
                (strstr(line," upgraded,")&&strstr(line," newly installed,"))){
            recognized++;
        }
        if(!start||blocked)continue;
        const char *end=start;while(*end&&!g_ascii_isspace(*end))end++;
        gchar *name=g_strndup(start,(gsize)(end-start));
        gchar *colon=strchr(name,':');if(colon)*colon='\0';
        if(package_is_kernel_blocked(name)){
            blocked=TRUE;if(blocked_name)*blocked_name=g_strdup(name);
        }
        g_free(name);
    }
    g_strfreev(lines);
    if(recognized_lines)*recognized_lines=recognized;
    return blocked?SIM_CHECK_KERNEL:SIM_CHECK_SAFE;
}

static int edpkg_probe_simulation(App *a,gchar **reason){
    if(reason)*reason=NULL;
    if(a->edpkg_sim_capability>0)return SIM_CHECK_SAFE;
    if(a->edpkg_sim_capability<0){
        if(reason)*reason=g_strdup("installed EDPKG does not advertise --simulate/--dry-run support");
        return SIM_CHECK_UNSUPPORTED;
    }
    const char *edpkg=edpkg_bin_path();
    if(!g_file_test(edpkg,G_FILE_TEST_IS_EXECUTABLE)){
        a->edpkg_sim_capability=-1;
        if(reason)*reason=g_strdup_printf("%s is not executable",edpkg);
        return SIM_CHECK_UNSUPPORTED;
    }
    /* Capability probes are about advertised features, not the program's
     * preferred help exit status.  Some apt/dpkg-style tools return non-zero
     * for usage/help while still printing perfectly valid option text. */
    char *av[]={(char*)edpkg,"--help",NULL};
    gint rc=0;gchar *err=NULL;gchar *out=capture_guard_argv(a,av,FALSE,&rc,&err);
    if(rc==130){g_free(out);g_free(err);return SIM_CHECK_CANCELLED;}
    if(out)g_strchomp(out);
    if(err)g_strchomp(err);
    gboolean supported=(out&&((strstr(out,"--simulate")!=NULL)||(strstr(out,"--dry-run")!=NULL)))||
                       (err&&((strstr(err,"--simulate")!=NULL)||(strstr(err,"--dry-run")!=NULL)));
    if(supported){
        a->edpkg_sim_capability=1;
        g_free(out);g_free(err);
        return SIM_CHECK_SAFE;
    }
    if((out&&*out)||(err&&*err)){
        a->edpkg_sim_capability=-1;
        if(reason)*reason=g_strdup_printf("installed EDPKG help does not advertise --simulate/--dry-run%s%s",
            err&&*err?": ":"",err&&*err?err:(out&&*out?out:""));
        g_free(out);g_free(err);
        return SIM_CHECK_UNSUPPORTED;
    }
    if(reason)*reason=g_strdup_printf("EDPKG capability probe produced no output (exit %d)",rc);
    g_free(out);g_free(err);
    return SIM_CHECK_FAILED;
}

static int apt_simulation_kernel_check(App *a,char *const argv[],gchar **blocked_name,gchar **reason){
    if(blocked_name)*blocked_name=NULL;
    if(reason)*reason=NULL;
    GPtrArray *sim=g_ptr_array_new();
    g_ptr_array_add(sim,argv[0]);g_ptr_array_add(sim,"-s");
    for(guint i=1;argv[i];i++)g_ptr_array_add(sim,argv[i]);
    g_ptr_array_add(sim,NULL);
    gint rc=0;gchar *err=NULL;
    gchar *out=capture_guard_argv(a,(gchar**)sim->pdata,FALSE,&rc,&err);g_ptr_array_free(sim,TRUE);
    if(rc==130){g_free(out);g_free(err);return SIM_CHECK_CANCELLED;}
    if(err)g_strchomp(err);
    if(rc!=0){
        if(reason)*reason=g_strdup_printf("APT simulation failed (exit %d)%s%s",rc,
            err&&*err?": ":"",err&&*err?err:"");
        g_free(out);g_free(err);return SIM_CHECK_FAILED;
    }
    guint recognized=0;int result=simulation_output_kernel_check(out,blocked_name,&recognized);
    if(result==SIM_CHECK_SAFE&&recognized==0){
        if(reason)*reason=g_strdup("APT simulation completed but its transaction plan was not recognized");
        result=SIM_CHECK_UNRECOGNIZED;
    }
    g_free(out);g_free(err);return result;
}

static int edpkg_simulation_kernel_check(App *a,const char *cmd,GPtrArray *names,
                                         gchar **blocked_name,gchar **reason){
    if(blocked_name)*blocked_name=NULL;
    if(reason)*reason=NULL;
    int cap=edpkg_probe_simulation(a,reason);if(cap!=SIM_CHECK_SAFE)return cap;
    GPtrArray *v=g_ptr_array_new();
    g_ptr_array_add(v,(gpointer)edpkg_bin_path());
    g_ptr_array_add(v,"--simulate");
    g_ptr_array_add(v,"-y");
    g_ptr_array_add(v,(gpointer)cmd);
    if(names)for(guint i=0;i<names->len;i++)g_ptr_array_add(v,g_ptr_array_index(names,i));
    g_ptr_array_add(v,NULL);
    gint rc=0;gchar *err=NULL;
    gchar*out=capture_guard_argv(a,(gchar**)v->pdata,FALSE,&rc,&err);g_ptr_array_free(v,TRUE);
    if(rc==130){g_free(out);g_free(err);return SIM_CHECK_CANCELLED;}
    if(err)g_strchomp(err);
    if(rc!=0){
        if(reason)*reason=g_strdup_printf("EDPKG simulation failed (exit %d)%s%s",rc,
            err&&*err?": ":"",err&&*err?err:"");
        g_free(out);g_free(err);return SIM_CHECK_FAILED;
    }
    guint recognized=0;int result=simulation_output_kernel_check(out,blocked_name,&recognized);
    if(result==SIM_CHECK_SAFE&&recognized==0){
        if(reason)*reason=g_strdup("EDPKG simulation completed but EDPKG GUI did not recognize its transaction-plan format");
        result=SIM_CHECK_UNRECOGNIZED;
    }
    g_free(out);g_free(err);return result;
}

/* Non-GTK regression entry point.  It intentionally exercises the same
 * capability probe and simulation parser used by real EDPKG transactions.
 * Tests override EDPKG_GUI_EDPKG_BIN with a fake engine. */
static int edpkg_guard_selftest(void){
    App a={0};
    GPtrArray *names=g_ptr_array_new();
    g_ptr_array_add(names,(gpointer)"essora-welcome");
    gchar *blocked=NULL,*reason=NULL;
    int check=edpkg_simulation_kernel_check(&a,"install",names,&blocked,&reason);
    g_ptr_array_free(names,TRUE);
    if(check==SIM_CHECK_SAFE){
        g_print("EDPKG guard selftest: SAFE\n");
        g_free(blocked);g_free(reason);
        return 0;
    }
    g_printerr("EDPKG guard selftest: FAIL check=%d%s%s\n",check,
        reason&&*reason?" reason=":"",reason&&*reason?reason:"");
    g_free(blocked);g_free(reason);
    return 1;
}

static int report_kernel_guard_result(App *a,int check,gchar *blocked,gchar *reason){
    if(check==SIM_CHECK_SAFE){g_free(blocked);g_free(reason);return 0;}
    gchar *msg=NULL;int rc=100;
    if(check==SIM_CHECK_KERNEL)
        msg=g_strdup_printf("Essora: package resolution would change protected kernel package '%s'; operation blocked.",blocked?blocked:"kernel");
    else if(check==SIM_CHECK_UNSUPPORTED)
        msg=g_strdup_printf("Essora: dependency guard unavailable: %s. Transaction blocked.",reason?reason:"simulation is not supported");
    else if(check==SIM_CHECK_UNRECOGNIZED)
        msg=g_strdup_printf("Essora: dependency guard could not understand the simulation plan: %s. Transaction blocked.",reason?reason:"unknown plan format");
    else if(check==SIM_CHECK_CANCELLED){
        msg=g_strdup("Dependency simulation cancelled.");rc=130;
    }else
        msg=g_strdup_printf("Essora: dependency simulation failed: %s. Transaction blocked.",reason?reason:"unknown error");
    progress_append_thread(a,msg);g_free(msg);g_free(blocked);g_free(reason);return rc;
}

static int run_apt_guarded(App *a,char *const argv[]){
    cancel_phase_safe(a);
    gchar *blocked=NULL,*reason=NULL;
    int check=apt_simulation_kernel_check(a,argv,&blocked,&reason);
    int denied=report_kernel_guard_result(a,check,blocked,reason);
    if(denied)return denied;
    cancel_phase_package_command(a);
    return run_pty(a,argv);
}

/* EDPKG has its own resolver and repository universe.  Its native --simulate
 * output is therefore the only authoritative dependency plan for EDPKG
 * transactions.  Never ask Debian apt-get to validate Essora-only packages. */
static int edpkg_dependency_guard(App *a,const char *cmd,GPtrArray *names){
    cancel_phase_safe(a);
    gchar *blocked=NULL,*reason=NULL;
    int check=edpkg_simulation_kernel_check(a,cmd,names,&blocked,&reason);
    return report_kernel_guard_result(a,check,blocked,reason);
}

static int run_backend_names(App*a,const char*cmd,GPtrArray*names){
    for(guint i=0;i<names->len;i++){
        const char *name=g_ptr_array_index(names,i);
        if(package_is_kernel_blocked(name)){
            progress_append_thread(a,"Essora: kernel package transaction blocked by EDPKG GUI.");
            return 100;
        }
    }
    if(a->backend==BACKEND_EDPKG){
        int guard=edpkg_dependency_guard(a,cmd,names);
        if(guard)return guard;
    }
    GPtrArray*v=g_ptr_array_new();
    if(a->backend==BACKEND_EDPKG){
        g_ptr_array_add(v,(gpointer)edpkg_bin_path());g_ptr_array_add(v,"-y");g_ptr_array_add(v,(gpointer)cmd);
    }else{
        g_ptr_array_add(v,(gpointer)APTGET_BIN);g_ptr_array_add(v,"-y");
        if(!strcmp(cmd,"reinstall")){g_ptr_array_add(v,"--reinstall");g_ptr_array_add(v,"install");}
        else g_ptr_array_add(v,(gpointer)cmd);
    }
    for(guint i=0;i<names->len;i++)g_ptr_array_add(v,g_ptr_array_index(names,i));
    g_ptr_array_add(v,NULL);
    int rc=0;
    if(a->backend==BACKEND_APT)rc=run_apt_guarded(a,(char*const*)v->pdata);
    else{cancel_phase_package_command(a);rc=run_pty(a,(char*const*)v->pdata);}
    g_ptr_array_free(v,TRUE);return rc;
}

/* EDPKG exposes fix-broken through its apt/apt-get compatibility frontend.
 * Run the same EDPKG executable with argv[0]="apt-get" so this works even on
 * a pure-EDPKG installation without a /usr/bin/apt-get symlink. */
static int edpkg_fix_simulation_kernel_check(App *a,gchar **blocked_name,gchar **reason){
    if(blocked_name)*blocked_name=NULL;
    if(reason)*reason=NULL;
    int cap=edpkg_probe_simulation(a,reason);if(cap!=SIM_CHECK_SAFE)return cap;
    char *av[]={(char*)edpkg_bin_path(),"apt-get","--simulate","-f","install","-y",NULL};
    gint rc=0;gchar *err=NULL;gchar*out=capture_guard_argv(a,av,TRUE,&rc,&err);
    if(rc==130){g_free(out);g_free(err);return SIM_CHECK_CANCELLED;}
    if(err)g_strchomp(err);
    if(rc!=0){
        if(reason)*reason=g_strdup_printf("EDPKG fix-broken simulation failed (exit %d)%s%s",rc,
            err&&*err?": ":"",err&&*err?err:"");
        g_free(out);g_free(err);return SIM_CHECK_FAILED;
    }
    guint recognized=0;int result=simulation_output_kernel_check(out,blocked_name,&recognized);
    if(result==SIM_CHECK_SAFE&&recognized==0){
        if(reason)*reason=g_strdup("EDPKG fix-broken simulation returned an unrecognized plan");
        result=SIM_CHECK_UNRECOGNIZED;
    }
    g_free(out);g_free(err);return result;
}

static int run_edpkg_fix_broken(App*a){
    cancel_phase_safe(a);
    gchar *blocked=NULL,*reason=NULL;int check=edpkg_fix_simulation_kernel_check(a,&blocked,&reason);
    int denied=report_kernel_guard_result(a,check,blocked,reason);if(denied)return denied;
    char *av[]={"apt-get","-y","-f","install",NULL};
    cancel_phase_package_command(a);
    return run_pty_exec(a,edpkg_bin_path(),av);
}

static int run_backend_update(App*a){
    if(a->backend==BACKEND_EDPKG){char*av[]={(char*)edpkg_bin_path(),"update",NULL};return run_pty(a,av);}
    char*av[]={(char*)APTGET_BIN,"update",NULL};return run_pty(a,av);
}
static int run_backend_clean(App*a){
    if(a->backend==BACKEND_EDPKG){char*av[]={(char*)edpkg_bin_path(),"clean",NULL};return run_pty(a,av);}
    char*av[]={(char*)APTGET_BIN,"clean",NULL};return run_pty(a,av);
}
static int run_backend_edit_sources(App*a){
    if(a->backend==BACKEND_EDPKG){char*av[]={(char*)edpkg_bin_path(),"edit-sources",NULL};return run_pty(a,av);}
    if(g_file_test(APT_BIN,G_FILE_TEST_IS_EXECUTABLE)){char*av[]={(char*)APT_BIN,"edit-sources",NULL};return run_pty(a,av);}
    progress_append_thread(a,"/usr/bin/apt is unavailable; edit /etc/apt/sources.list manually.");return 127;
}

static gpointer worker(gpointer d){
    Job*j=d;App*a=j->a;int rc=0;
    if(j->type==JOB_UPDATE)rc=run_backend_update(a);
    else if(j->type==JOB_CLEAN)rc=run_backend_clean(a);
    else if(j->type==JOB_EDIT_SOURCES)rc=run_backend_edit_sources(a);
    else if(j->type==JOB_APPLY){
        if(j->remove&&j->remove->len)rc=run_backend_names(a,"remove",j->remove);
        if(!rc&&j->install&&j->install->len)rc=run_backend_names(a,"install",j->install);
        if(!rc&&j->reinstall&&j->reinstall->len)rc=run_backend_names(a,"reinstall",j->reinstall);
    }else if(j->type==JOB_FIX){
        if(j->update_first)rc=run_backend_update(a);
        if(!rc){
            progress_append_thread(a,"--- configure pending packages ---");
            /* Configuration is already a mutating phase: do not offer a
             * deceptively safe Cancel button once we enter it. */
            g_atomic_int_set(&a->cancel_download_mode,2);
            g_atomic_int_set(&a->cancel_download_seen,0);
            if(!g_atomic_int_exchange(&a->cancel_locked,1))g_idle_add(cancel_lock_idle,a);
            if(a->backend==BACKEND_EDPKG){char*av[]={(char*)edpkg_bin_path(),"configure","-a",NULL};rc=run_pty(a,av);}
            else{char*av[]={(char*)DPKG_BIN,"--configure","-a",NULL};rc=run_pty(a,av);}
        }
        if(!rc){
            progress_append_thread(a,"--- fix broken dependencies ---");
            if(a->backend==BACKEND_EDPKG)rc=run_edpkg_fix_broken(a);
            else{char*av[]={(char*)APTGET_BIN,"-f","install","-y",NULL};rc=run_apt_guarded(a,av);}
        }
        if(!rc&&j->autoremove){
            progress_append_thread(a,"--- autoremove ---");
            if(a->backend==BACKEND_EDPKG){
                cancel_phase_safe(a);
                gchar*blocked=NULL,*reason=NULL;
                int check=edpkg_simulation_kernel_check(a,"autoremove",NULL,&blocked,&reason);
                rc=report_kernel_guard_result(a,check,blocked,reason);
                if(!rc){char*av[]={(char*)edpkg_bin_path(),"-y","autoremove",NULL};cancel_phase_package_command(a);rc=run_pty(a,av);}
            }else{
                char*av[]={(char*)APTGET_BIN,"-y","autoremove",NULL};rc=run_apt_guarded(a,av);
            }
        }
    }
    gboolean cancelled=(rc==130)||g_atomic_int_get(&a->cancel_requested);
    GHashTable *local_state=NULL;
    if((rc==0||cancelled)&&(j->type==JOB_APPLY||j->type==JOB_FIX))
        local_state=capture_local_state_snapshot(a);
    /* Success closes automatically. Cancellation and errors stay open so the
     * user can see exactly where the backend stopped. */
    finish_thread(a,rc==0,cancelled,TRUE,j->type==JOB_UPDATE,
        cancelled?tr(T_CANCELLED):(rc==0?"":"Exit code indicates an error."),local_state);
    job_free(j);return NULL;
}
static Job *job_new(App*a,JobType t){Job*j=g_new0(Job,1);j->a=a;j->type=t;return j;}
static void launch_job(App*a,Job*j,const char*title){
    gboolean guarded=(j->type==JOB_APPLY||j->type==JOB_FIX);
    g_atomic_int_set(&a->cancel_requested,0);
    g_atomic_int_set(&a->cancel_locked,guarded?1:0);
    g_atomic_int_set(&a->cancel_download_mode,guarded?1:0);
    g_atomic_int_set(&a->cancel_download_seen,0);
    g_atomic_int_set(&a->active_child_pid,0);
    start_busy(a,title);
    if(guarded&&a->progress_cancel)gtk_widget_set_sensitive(a->progress_cancel,FALSE);
    a->worker_thread=g_thread_new("edpkg-gui-worker",worker,j);
}

static void on_refresh(GtkButton*b,gpointer d){
    (void)b;App*a=d;
    if(!check_root_or_warn(a))return;
    gtk_label_set_text(GTK_LABEL(a->status_label),tr(T_REFRESHING_REPOS));
    launch_job(a,job_new(a,JOB_UPDATE),tr(T_REFRESHING_REPOS));
    if(a->progress_label)gtk_label_set_text(GTK_LABEL(a->progress_label),tr(T_REFRESHING_REPOS));
}
static void on_clean(GtkButton*b,gpointer d){(void)b;App*a=d;if(!check_root_or_warn(a))return;GtkWidget*q=gtk_message_dialog_new(GTK_WINDOW(a->window),GTK_DIALOG_MODAL,GTK_MESSAGE_QUESTION,GTK_BUTTONS_OK_CANCEL,"%s",tr(T_CLEAN_CONFIRM));int r=gtk_dialog_run(GTK_DIALOG(q));gtk_widget_destroy(q);if(r==GTK_RESPONSE_OK)launch_job(a,job_new(a,JOB_CLEAN),tr(T_CLEAN));}
static void on_edit_sources(GtkButton*b,gpointer d){(void)b;App*a=d;if(!check_root_or_warn(a))return;launch_job(a,job_new(a,JOB_EDIT_SOURCES),tr(T_REPOSITORIES));}

static void on_mark_upgrades(GtkButton*b,gpointer d){
    (void)b;App*a=d;
    GHashTableIter hi;gpointer k,v;
    g_hash_table_iter_init(&hi,a->packages);
    while(g_hash_table_iter_next(&hi,&k,&v)){
        Package*p=v;
        if(p->is_upgradable&&!package_is_kernel_blocked(p->name))p->action=ACT_UPGRADE;
    }
    rebuild_model(a);update_summary(a);refresh_details(a);
}

/* A scrollable confirmation keeps Cancel/Apply pinned outside the package list.
 * A GtkMessageDialog with the whole list as secondary text grows beyond the
 * screen as soon as "Mark upgrades" selects a large repository batch. */
static gboolean confirm_deb_changes(App *a){
    GtkWidget *dlg=gtk_dialog_new_with_buttons(tr(T_APPLY_TITLE),GTK_WINDOW(a->window),
        GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        tr(T_CANCEL),GTK_RESPONSE_CANCEL,tr(T_APPLY),GTK_RESPONSE_OK,NULL);
    gtk_window_set_position(GTK_WINDOW(dlg),GTK_WIN_POS_CENTER_ON_PARENT);
    GtkWidget *area=gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
    gtk_container_set_border_width(GTK_CONTAINER(box),14);
    gtk_box_pack_start(GTK_BOX(area),box,TRUE,TRUE,0);
    gchar *heading=g_strdup_printf("%s (%u)",tr(T_CONFIRM_APPLY),marked_count(a));
    GtkWidget *label=gtk_label_new(heading);
    g_free(heading);
    gtk_label_set_xalign(GTK_LABEL(label),0);
    gtk_label_set_line_wrap(GTK_LABEL(label),TRUE);
    gtk_box_pack_start(GTK_BOX(box),label,FALSE,FALSE,0);
    GtkWidget *sc=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc),GTK_SHADOW_IN);
    gtk_widget_set_size_request(sc,200,120);
    gtk_box_pack_start(GTK_BOX(box),sc,TRUE,TRUE,0);
    GtkWidget *list=gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(list),FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(list),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(list),GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(list),10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(list),10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(list),8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(list),8);
    gtk_container_add(GTK_CONTAINER(sc),list);
    GString *names=g_string_sized_new(4096);
    GHashTableIter hi;gpointer k,v;
    g_hash_table_iter_init(&hi,a->packages);
    while(g_hash_table_iter_next(&hi,&k,&v)){
        Package *p=v;
        if(p->action!=ACT_NONE)g_string_append_printf(names,"• %s — %s\n",p->name,action_text(p->action));
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(list)),names->str,-1);
    g_string_free(names,TRUE);
    /* Work area, not total monitor size, so Fluxbox panels do not hide buttons. */
    gint width=740,height=440;
    GdkWindow *parent_window=gtk_widget_get_window(a->window);
    if(parent_window){
        GdkDisplay *display=gdk_window_get_display(parent_window);
        GdkMonitor *monitor=gdk_display_get_monitor_at_window(display,parent_window);
        if(monitor){
            GdkRectangle workarea;
            gdk_monitor_get_workarea(monitor,&workarea);
            width=MIN(width,MAX(300,workarea.width-80));
            height=MIN(height,MAX(200,workarea.height-80));
        }
    }
    gtk_window_set_default_size(GTK_WINDOW(dlg),width,height);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg),GTK_RESPONSE_OK);
    gtk_widget_show_all(dlg);
    gint result=gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return result==GTK_RESPONSE_OK;
}

static void on_apply(GtkButton*b,gpointer d){
    (void)b;App*a=d;
    eapt_debug_log("DEB on_apply busy=%d loading=%d marked=%u",a->busy,a->loading,marked_count(a));
    eapt_debug_toplevels("DEB on_apply");
    if(!check_root_or_warn(a))return;
    if(!marked_count(a)){
        GtkWidget*q=gtk_message_dialog_new(GTK_WINDOW(a->window),GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO,GTK_BUTTONS_CLOSE,"%s",tr(T_NO_MARKS));
        gtk_dialog_run(GTK_DIALOG(q));gtk_widget_destroy(q);return;
    }
    if(!confirm_deb_changes(a))return;
    Job*j=job_new(a,JOB_APPLY);
    j->install=g_ptr_array_new_with_free_func(g_free);
    j->remove=g_ptr_array_new_with_free_func(g_free);
    j->reinstall=g_ptr_array_new_with_free_func(g_free);
    GHashTableIter hi;gpointer k,v;g_hash_table_iter_init(&hi,a->packages);
    while(g_hash_table_iter_next(&hi,&k,&v)){
        Package*p=v;
        if(!package_is_kernel_blocked(p->name)){
            switch(p->action){
            case ACT_INSTALL:case ACT_UPGRADE:g_ptr_array_add(j->install,g_strdup(p->name));break;
            case ACT_REMOVE:g_ptr_array_add(j->remove,g_strdup(p->name));break;
            case ACT_REINSTALL:g_ptr_array_add(j->reinstall,g_strdup(p->name));break;
            default:break;
            }
        }
    }
    launch_job(a,j,tr(T_APPLY_TITLE));
}

static void on_fix(GtkButton*b,gpointer d){(void)b;App*a=d;if(!check_root_or_warn(a))return;GtkWidget*dlg=gtk_dialog_new_with_buttons(tr(T_FIX_TITLE),GTK_WINDOW(a->window),GTK_DIALOG_MODAL,tr(T_CANCEL),GTK_RESPONSE_CANCEL,tr(T_RUN_REPAIR),GTK_RESPONSE_OK,NULL);GtkWidget*box=gtk_dialog_get_content_area(GTK_DIALOG(dlg));GtkWidget*v=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);gtk_container_set_border_width(GTK_CONTAINER(v),14);gtk_box_pack_start(GTK_BOX(box),v,TRUE,TRUE,0);GtkWidget*u=gtk_check_button_new_with_label(tr(T_UPDATE_FIRST));GtkWidget*r=gtk_check_button_new_with_label(tr(T_AUTOREMOVE_AFTER));gtk_box_pack_start(GTK_BOX(v),u,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(v),r,FALSE,FALSE,0);gtk_widget_show_all(dlg);int res=gtk_dialog_run(GTK_DIALOG(dlg));gboolean du=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u)),dr=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r));gtk_widget_destroy(dlg);if(res==GTK_RESPONSE_OK){Job*j=job_new(a,JOB_FIX);j->update_first=du;j->autoremove=dr;launch_job(a,j,tr(T_FIX_TITLE));}}

static gboolean search_refilter_idle(gpointer d){
    App*a=d;a->search_debounce_id=0;
    gchar*q=g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(a->search)),-1);
    glong n=q?g_utf8_strlen(q,-1):0;
    glong prev=a->search_folded?g_utf8_strlen(a->search_folded,-1):0;
    if(n==1&&prev<=1){g_free(q);return G_SOURCE_REMOVE;}
    g_free(a->search_folded);a->search_folded=q;
    if(n>=2){
        /* A typed search is global: never require the user to click “All” first. */
        a->filter_mode=F_ALL;
        if(!a->catalog_loaded&&!a->loading)reload_data_async(a,FALSE);
    }
    rebuild_model(a);refresh_details(a);return G_SOURCE_REMOVE;
}
static void on_search_changed(GtkEditable*e,gpointer d){(void)e;App*a=d;if(a->search_debounce_id)g_source_remove(a->search_debounce_id);a->search_debounce_id=g_timeout_add(120,search_refilter_idle,a);}
static void on_selection_changed(GtkTreeSelection*s,gpointer d){(void)s;refresh_details(d);}
static void on_install_mark(GtkButton*b,gpointer d){(void)b;set_selected_action(d,ACT_INSTALL);}static void on_remove_mark(GtkButton*b,gpointer d){(void)b;set_selected_action(d,ACT_REMOVE);}static void on_reinstall_mark(GtkButton*b,gpointer d){(void)b;set_selected_action(d,ACT_REINSTALL);}static void on_clear_mark(GtkButton*b,gpointer d){(void)b;set_selected_action(d,ACT_NONE);}

static void set_filter(App*a,FilterMode f){a->filter_mode=f;if(f!=F_INSTALLED&&f!=F_BROKEN&&!a->catalog_loaded&&!a->loading)reload_data_async(a,FALSE);rebuild_model(a);refresh_details(a);}
static void filter_clicked(GtkButton*b,gpointer d){App*a=d;gpointer p=g_object_get_data(G_OBJECT(b),"filter-id");set_filter(a,(FilterMode)GPOINTER_TO_INT(p));}

static GtkWidget *sidebar_button(App*a,const char*label,FilterMode f){GtkWidget*b=gtk_button_new_with_label(label);gtk_widget_set_name(b,"side-button");gtk_widget_set_halign(b,GTK_ALIGN_FILL);g_object_set_data(G_OBJECT(b),"filter-id",GINT_TO_POINTER(f));g_signal_connect(b,"clicked",G_CALLBACK(filter_clicked),a);return b;}

static GtkWidget *icon_button(const char *icon,const char *label){GtkWidget*b=gtk_button_new();GtkWidget*h=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);GtkWidget*i=gtk_image_new_from_icon_name(icon,GTK_ICON_SIZE_BUTTON);GtkWidget*l=gtk_label_new(label);gtk_container_add(GTK_CONTAINER(b),h);gtk_box_pack_start(GTK_BOX(h),i,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(h),l,FALSE,FALSE,0);return b;}

static void add_text_column(App*a,const char*title,int col,int width){
    GtkCellRenderer*r=gtk_cell_renderer_text_new();
    g_object_set(r,"ellipsize",PANGO_ELLIPSIZE_END,NULL);
    GtkTreeViewColumn*c=gtk_tree_view_column_new_with_attributes(title,r,"text",col,
        "cell-background",C_BG,"cell-background-set",C_HAS_COLOR,
        "foreground",C_FG,"foreground-set",C_HAS_COLOR,NULL);
    gtk_tree_view_column_set_resizable(c,TRUE);gtk_tree_view_column_set_sizing(c,GTK_TREE_VIEW_COLUMN_FIXED);
    if(width>0) gtk_tree_view_column_set_fixed_width(c,width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->tree),c);
}

static void add_state_icon_column(App*a,const char*title,int width){
    GtkCellRenderer*r=gtk_cell_renderer_pixbuf_new();g_object_set(r,"xalign",0.5,NULL);
    gtk_cell_renderer_set_fixed_size(r,28,28);
    GtkTreeViewColumn*c=gtk_tree_view_column_new_with_attributes(title,r,"pixbuf",C_STATE_ICON,
        "cell-background",C_BG,"cell-background-set",C_HAS_COLOR,NULL);
    gtk_tree_view_column_set_sizing(c,GTK_TREE_VIEW_COLUMN_FIXED);gtk_tree_view_column_set_fixed_width(c,width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->tree),c);
}

static void load_css(void){static const char*css=
"window, dialog { background:#0b1d29; color:#d8e2ea; }"
"messagedialog, messagedialog box, messagedialog label { background:#0b1d29; color:#d8e2ea; }"
"#brand { background:#0d202d; padding:10px 14px; }"
"#brand-title { color:#e4edf3; font-size:16px; font-weight:bold; }"
"#brand-subtitle { color:#91a5b4; font-size:11px; }"
"#accent { background:#236648; min-height:2px; }"
"#toolbar { background:#102634; padding:8px 10px; }"
"button { background:#17364d; color:#dce6ed; border:1px solid #21475f; border-radius:0; padding:6px 10px; }"
"button:hover { background:#1d4862; }"
"#apply { background:#236648; border-color:#2d7b59; }"
"#apply:hover { background:#2c7b59; }"
"#side { background:#0d202d; border-right:1px solid #1d3c4d; padding:8px; }"
"#side-button { background:transparent; border:0; padding:8px 10px; }"
"#side-button:hover { background:#163344; }"
"entry { background:#091923; color:#e0e8ee; border:1px solid #21465d; border-radius:0; padding:6px; }"
"treeview { background:#0e2230; color:#d9e2e8; }"
"treeview:selected { background:#315f7a; color:white; }"
"#package-tabs header { background:#0d202d; border-bottom:1px solid #1d3c4d; }"
"#package-tabs tab { background:#102634; color:#9eb2c0; padding:6px 14px; }"
"#package-tabs tab:checked { background:#17364d; color:#ffffff; border-bottom:2px solid #7D9B37; }"
"treeview header button { background:#102b3b; color:#b7c7d2; padding:5px; }"
"#details { background:#102634; border-top:1px solid #236648; padding:10px 12px; }"
"#details-title { font-size:14px; }"
"#dim { color:#90a4b3; }"
"#statusbar { background:#081823; color:#90a4b3; border-top:1px solid #1c394a; padding:5px 10px; }"
"textview, textview text { background:#07141d; color:#d5e1e9; }"
"progressbar trough { background:#122c3a; min-height:7px; }"
"progressbar progress { background:#236648; min-height:7px; }";
    GtkCssProvider*p=gtk_css_provider_new();gtk_css_provider_load_from_data(p,css,-1,NULL);gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(p),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);g_object_unref(p);
}

static GdkPixbuf *load_icon_file(const char *path, int size){
    GError*e=NULL;
    GdkPixbuf*pb=gdk_pixbuf_new_from_file_at_scale(path,size,size,TRUE,&e);
    if(e){g_printerr("edpkg-gui: cannot load icon %s: %s\n",path,e->message);g_error_free(e);}
    return pb;
}

static GdkPixbuf *load_icon_pair(const char *png_path,const char *svg_path,int size){
    GdkPixbuf *pb=NULL;
    if(png_path&&g_file_test(png_path,G_FILE_TEST_IS_REGULAR)) pb=load_icon_file(png_path,size);
    if(!pb&&svg_path&&g_file_test(svg_path,G_FILE_TEST_IS_REGULAR)) pb=load_icon_file(svg_path,size);
    return pb;
}

static void load_state_icons(App*a){
    a->icon_installed=load_icon_pair(ICON_INSTALLED_FILE,GUI_LIBDIR "/package-installed.svg",24);
    a->icon_broken=load_icon_pair(ICON_BROKEN_FILE,GUI_LIBDIR "/package-broken.svg",24);
    a->icon_upgradable=load_icon_pair(ICON_UPGRADABLE_FILE,GUI_LIBDIR "/package-upgradable.svg",24);
}

static GtkWidget *manager_icon(GtkIconSize size){
    gint w=48,h=48;gtk_icon_size_lookup(size,&w,&h);
    GdkPixbuf *pb=load_icon_pair(ICON_FILE,ICON_SVG_FILE,MAX(w,h));
    if(pb){GtkWidget *img=gtk_image_new_from_pixbuf(pb);g_object_unref(pb);return img;}
    return gtk_image_new_from_icon_name("package-x-generic",size);
}

static void set_window_manager_icon(GtkWindow*w){
    GdkPixbuf *pb=load_icon_pair(ICON_FILE,ICON_SVG_FILE,64);
    if(pb){gtk_window_set_icon(w,pb);g_object_unref(pb);}
}

static void about_clicked(GtkButton*b,gpointer d){
    (void)b;App*a=d;GtkWidget*dlg=gtk_about_dialog_new();
    set_window_manager_icon(GTK_WINDOW(dlg));
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dlg),a->backend==BACKEND_APT?"Essora APT/dpkg GUI":"EDPKG GUI");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dlg),APP_VERSION);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dlg),tr(T_SUBTITLE));
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dlg),"https://www.essora.org");
    GdkPixbuf*logo=load_icon_pair(ICON_FILE,ICON_SVG_FILE,96);
    if(logo){gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dlg),logo);g_object_unref(logo);}
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dlg),GTK_LICENSE_GPL_3_0);
    const gchar *thanks[] = {"Ivan (ivan-hc) — AM Application Manager, v10.6", "https://github.com/ivan-hc/AM", NULL};
    gtk_about_dialog_add_credit_section(GTK_ABOUT_DIALOG(dlg),"AppImage engine (GPL-3.0)", thanks);
    gtk_window_set_transient_for(GTK_WINDOW(dlg),GTK_WINDOW(a->window));
    gtk_dialog_run(GTK_DIALOG(dlg));gtk_widget_destroy(dlg);
}


/* AppImage initializes only on its first visit: no network or subprocess work at startup. */
static GtkWidget *essora_tab_label(const gchar *name, const gchar *icon_file) {
    GtkWidget *tab=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,5);
    GError *err=NULL;
    GdkPixbuf *pix=gdk_pixbuf_new_from_file_at_scale(icon_file,24,24,TRUE,&err);
    if (pix) {
        GtkWidget *img=gtk_image_new_from_pixbuf(pix);
        gtk_box_pack_start(GTK_BOX(tab),img,FALSE,FALSE,0);
        g_object_unref(pix);
    }
    if (err) g_error_free(err);
    GtkWidget *label=gtk_label_new(name);
    gtk_box_pack_start(GTK_BOX(tab),label,FALSE,FALSE,0);
    gtk_widget_show_all(tab);
    return tab;
}
static void essora_switch_tab(GtkNotebook *notebook,GtkWidget *page,guint index,gpointer userdata) {
    (void)notebook;(void)index;
    App *a=userdata;
    if (!g_object_get_data(G_OBJECT(page),"essora-appimage-lazy")) return;
    g_object_set_data(G_OBJECT(page),"essora-appimage-lazy",NULL);
    GtkWidget *view=appimage_page_new(GTK_WINDOW(a->window),a->is_root);
    gtk_box_pack_start(GTK_BOX(page),view,TRUE,TRUE,0);
    gtk_widget_show_all(view);
}

static void build_ui(App*a){
    load_state_icons(a);
    a->window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(a->window),a->backend==BACKEND_APT?"APT/dpkg — Essora":tr(T_APP_TITLE));set_window_manager_icon(GTK_WINDOW(a->window));gtk_window_set_default_size(GTK_WINDOW(a->window),1000,680);gtk_window_set_position(GTK_WINDOW(a->window),GTK_WIN_POS_CENTER);eapt_debug_watch_window(a->window,"MAIN");g_signal_connect(a->window,"delete-event",G_CALLBACK(main_delete_event),a);g_signal_connect(a->window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    GtkWidget*root=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_container_add(GTK_CONTAINER(a->window),root);
    GtkWidget*brand=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);gtk_widget_set_name(brand,"brand");GtkWidget*ico=manager_icon(GTK_ICON_SIZE_DIALOG);gtk_box_pack_start(GTK_BOX(brand),ico,FALSE,FALSE,0);GtkWidget*bv=gtk_box_new(GTK_ORIENTATION_VERTICAL,1);GtkWidget*bt=gtk_label_new(backend_name(a));gtk_widget_set_name(bt,"brand-title");gtk_label_set_xalign(GTK_LABEL(bt),0);GtkWidget*bs=gtk_label_new(tr(T_SUBTITLE));gtk_widget_set_name(bs,"brand-subtitle");gtk_label_set_xalign(GTK_LABEL(bs),0);gtk_box_pack_start(GTK_BOX(bv),bt,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(bv),bs,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(brand),bv,TRUE,TRUE,0);GtkWidget*about=gtk_button_new_with_label(tr(T_ABOUT));g_signal_connect(about,"clicked",G_CALLBACK(about_clicked),a);gtk_box_pack_end(GTK_BOX(brand),about,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(root),brand,FALSE,FALSE,0);GtkWidget*accent=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);gtk_widget_set_name(accent,"accent");gtk_box_pack_start(GTK_BOX(root),accent,FALSE,FALSE,0);
    a->is_root=(geteuid()==0);GtkWidget*notebook=gtk_notebook_new();gtk_widget_set_name(notebook,"package-tabs");gtk_box_pack_start(GTK_BOX(root),notebook,TRUE,TRUE,0);GtkWidget*deb_page=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_notebook_append_page(GTK_NOTEBOOK(notebook),deb_page,
        essora_tab_label("DEB",GUI_LIBDIR "/edpkg-manager.png"));
    GtkWidget*tb=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,7);gtk_widget_set_name(tb,"toolbar");a->btn_refresh=icon_button("view-refresh",tr(T_REFRESH));a->btn_mark_upgrades=icon_button("software-update-available",tr(T_MARK_UPGRADES));a->btn_apply=icon_button("emblem-default",tr(T_APPLY));gtk_widget_set_name(a->btn_apply,"apply");a->btn_fix=icon_button("tools-check-spelling",tr(T_FIX));a->btn_clean=icon_button("edit-clear",tr(T_CLEAN));GtkWidget*repo=icon_button("document-edit",tr(T_REPOSITORIES));gtk_box_pack_start(GTK_BOX(tb),a->btn_refresh,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tb),a->btn_mark_upgrades,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tb),a->btn_apply,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tb),a->btn_fix,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tb),a->btn_clean,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tb),repo,FALSE,FALSE,0);a->search=gtk_search_entry_new();gtk_entry_set_placeholder_text(GTK_ENTRY(a->search),tr(T_SEARCH));gtk_widget_set_size_request(a->search,260,-1);gtk_box_pack_end(GTK_BOX(tb),a->search,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(deb_page),tb,FALSE,FALSE,0);
    g_signal_connect(a->btn_refresh,"clicked",G_CALLBACK(on_refresh),a);g_signal_connect(a->btn_mark_upgrades,"clicked",G_CALLBACK(on_mark_upgrades),a);g_signal_connect(a->btn_apply,"clicked",G_CALLBACK(on_apply),a);g_signal_connect(a->btn_fix,"clicked",G_CALLBACK(on_fix),a);g_signal_connect(a->btn_clean,"clicked",G_CALLBACK(on_clean),a);g_signal_connect(repo,"clicked",G_CALLBACK(on_edit_sources),a);g_signal_connect(a->search,"changed",G_CALLBACK(on_search_changed),a);
    a->repo_notice=gtk_info_bar_new();
    gtk_info_bar_set_message_type(GTK_INFO_BAR(a->repo_notice),GTK_MESSAGE_INFO);
    GtkWidget*repo_msg=gtk_label_new(tr(T_REPO_NOTICE));
    gtk_label_set_xalign(GTK_LABEL(repo_msg),0);
    gtk_label_set_line_wrap(GTK_LABEL(repo_msg),TRUE);
    gtk_widget_set_margin_start(repo_msg,8);
    gtk_widget_set_margin_end(repo_msg,8);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(a->repo_notice))),repo_msg);
    gtk_box_pack_start(GTK_BOX(deb_page),a->repo_notice,FALSE,FALSE,0);
    GtkWidget*pan=gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);gtk_box_pack_start(GTK_BOX(deb_page),pan,TRUE,TRUE,0);GtkWidget*side=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);gtk_widget_set_name(side,"side");gtk_widget_set_size_request(side,180,-1);a->filter_buttons[F_ALL]=sidebar_button(a,tr(T_ALL),F_ALL);a->filter_buttons[F_INSTALLED]=sidebar_button(a,tr(T_INSTALLED),F_INSTALLED);a->filter_buttons[F_UPGRADABLE]=sidebar_button(a,tr(T_UPGRADABLE),F_UPGRADABLE);a->filter_buttons[F_NOT_INSTALLED]=sidebar_button(a,tr(T_NOT_INSTALLED),F_NOT_INSTALLED);a->filter_buttons[F_BROKEN]=sidebar_button(a,tr(T_BROKEN),F_BROKEN);for(int fi=0;fi<5;fi++)gtk_box_pack_start(GTK_BOX(side),a->filter_buttons[fi],FALSE,FALSE,0);gtk_paned_pack1(GTK_PANED(pan),side,FALSE,FALSE);
    GtkWidget*right=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);gtk_paned_pack2(GTK_PANED(pan),right,TRUE,FALSE);a->store=gtk_list_store_new(N_COLS,G_TYPE_STRING,GDK_TYPE_PIXBUF,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_BOOLEAN);a->tree=gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a->tree),TRUE);gtk_tree_view_set_enable_search(GTK_TREE_VIEW(a->tree),FALSE);gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(a->tree),TRUE);add_text_column(a,tr(T_ACTION),C_ACTION,90);add_state_icon_column(a,tr(T_STATE),72);add_text_column(a,tr(T_PACKAGE),C_NAME,230);add_text_column(a,tr(T_INSTALLED_VER),C_INSTALLED,145);add_text_column(a,tr(T_CANDIDATE),C_CANDIDATE,175);add_text_column(a,tr(T_ARCH),C_ARCH,110);GtkWidget*sc=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);gtk_container_add(GTK_CONTAINER(sc),a->tree);gtk_box_pack_start(GTK_BOX(right),sc,TRUE,TRUE,0);GtkTreeSelection*sel=gtk_tree_view_get_selection(GTK_TREE_VIEW(a->tree));gtk_tree_selection_set_mode(sel,GTK_SELECTION_MULTIPLE);g_signal_connect(sel,"changed",G_CALLBACK(on_selection_changed),a);
    GtkWidget*det=gtk_box_new(GTK_ORIENTATION_VERTICAL,5);gtk_widget_set_name(det,"details");a->detail_name=gtk_label_new(tr(T_NO_SELECTION));gtk_widget_set_name(a->detail_name,"details-title");gtk_label_set_xalign(GTK_LABEL(a->detail_name),0);a->detail_desc=gtk_label_new("");gtk_label_set_xalign(GTK_LABEL(a->detail_desc),0);gtk_label_set_line_wrap(GTK_LABEL(a->detail_desc),TRUE);a->detail_versions=gtk_label_new("");gtk_widget_set_name(a->detail_versions,"dim");gtk_label_set_xalign(GTK_LABEL(a->detail_versions),0);gtk_box_pack_start(GTK_BOX(det),a->detail_name,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(det),a->detail_desc,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(det),a->detail_versions,FALSE,FALSE,0);GtkWidget*db=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);a->btn_install=gtk_button_new_with_label(tr(T_MARK_INSTALL));a->btn_remove=gtk_button_new_with_label(tr(T_MARK_REMOVE));a->btn_reinstall=gtk_button_new_with_label(tr(T_MARK_REINSTALL));a->btn_clear=gtk_button_new_with_label(tr(T_CLEAR_MARK));gtk_box_pack_start(GTK_BOX(db),a->btn_install,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(db),a->btn_remove,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(db),a->btn_reinstall,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(db),a->btn_clear,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(det),db,FALSE,FALSE,0);g_signal_connect(a->btn_install,"clicked",G_CALLBACK(on_install_mark),a);g_signal_connect(a->btn_remove,"clicked",G_CALLBACK(on_remove_mark),a);g_signal_connect(a->btn_reinstall,"clicked",G_CALLBACK(on_reinstall_mark),a);g_signal_connect(a->btn_clear,"clicked",G_CALLBACK(on_clear_mark),a);gtk_box_pack_start(GTK_BOX(right),det,FALSE,FALSE,0);
    a->status_label=gtk_label_new(tr(T_READY));gtk_widget_set_name(a->status_label,"statusbar");gtk_label_set_xalign(GTK_LABEL(a->status_label),0);gtk_label_set_line_wrap(GTK_LABEL(a->status_label),TRUE);gtk_label_set_line_wrap_mode(GTK_LABEL(a->status_label),PANGO_WRAP_WORD_CHAR);gtk_box_pack_start(GTK_BOX(deb_page),a->status_label,FALSE,FALSE,0);
    gtk_widget_set_sensitive(a->btn_refresh,TRUE);gtk_widget_set_sensitive(a->btn_apply,a->is_root);gtk_widget_set_sensitive(a->btn_fix,a->is_root);gtk_widget_set_sensitive(a->btn_clean,a->is_root);gtk_widget_set_sensitive(repo,a->is_root);refresh_details(a);GtkWidget*flatpak_page=flatpak_page_new(GTK_WINDOW(a->window),a->is_root);gtk_notebook_append_page(GTK_NOTEBOOK(notebook),flatpak_page,
        essora_tab_label("Flatpak",GUI_LIBDIR "/essora-flatpak-64.png"));
    GtkWidget *appimage_holder=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),appimage_holder,
        essora_tab_label("AppImage",GUI_LIBDIR "/essora-appimage-64.png"));
    g_object_set_data(G_OBJECT(appimage_holder),"essora-appimage-lazy",GINT_TO_POINTER(1));
    g_signal_connect(notebook,"switch-page",G_CALLBACK(essora_switch_tab),a);
}

static int safe_download_selftest(void){
    static const char *safe[]={
        "Get:1 https://deb.devuan.org/merged excalibur/main amd64 pkg amd64 1.0 [42 kB]",
        "Hit:2 http://deb.devuan.org/merged excalibur InRelease",
        "Des:3 https://www.essora.org/repo ./ Packages [12 kB]",
        "Obj:4 http://deb.devuan.org/merged excalibur-security InRelease",
        "Ign:5 file:/var/cache/apt ./ Packages",
        NULL
    };
    static const char *unsafe[]={
        "Fetched 85.3 kB in 1s",
        "Descargados 85,3 kB en 1s",
        "Unpacking foo (1.0) ...",
        "Setting up foo (1.0) ...",
        "Err: http://broken.example/repo",
        NULL
    };
    for(guint i=0;safe[i];i++)if(!line_is_safe_download(safe[i])){
        g_printerr("safe-download selftest missed: %s\n",safe[i]);return 1;
    }
    for(guint i=0;unsafe[i];i++)if(line_is_safe_download(unsafe[i])){
        g_printerr("safe-download selftest false positive: %s\n",unsafe[i]);return 1;
    }
    g_print("safe-download selftest: OK\n");
    return 0;
}

static int edpkg_path_policy_selftest(void){
    const char *fake="/tmp/edpkg-gui-selftest-fake";
    g_setenv("EDPKG_GUI_EDPKG_BIN",fake,TRUE);
    edpkg_test_override_allowed=FALSE;
    if(strcmp(edpkg_bin_path(),EDPKG_BIN)!=0){
        g_printerr("EDPKG override leaked into normal mode\n");return 1;
    }
    edpkg_test_override_allowed=TRUE;
    if(strcmp(edpkg_bin_path(),fake)!=0){
        g_printerr("EDPKG override unavailable in selftest mode\n");return 1;
    }
    edpkg_test_override_allowed=FALSE;
    g_unsetenv("EDPKG_GUI_EDPKG_BIN");
    g_print("EDPKG path policy selftest: OK\n");
    return 0;
}

int main(int argc,char**argv){
    if(argc==2&&!strcmp(argv[1],"--selftest-edpkg-guard")){
        edpkg_test_override_allowed=TRUE;
        int rc=edpkg_guard_selftest();
        edpkg_test_override_allowed=FALSE;
        return rc;
    }
    if(argc==2&&!strcmp(argv[1],"--selftest-safe-download"))return safe_download_selftest();
    if(argc==2&&!strcmp(argv[1],"--selftest-edpkg-path-policy"))return edpkg_path_policy_selftest();
    gtk_init(&argc,&argv);
    eapt_debug_session_start(argc,argv);
    choose_language();load_css();
    GError*icon_err=NULL;
    if(!gtk_window_set_default_icon_from_file(ICON_FILE,&icon_err)){if(icon_err){g_printerr("edpkg-gui: cannot set default icon: %s\n",icon_err->message);eapt_debug_log("default icon error: %s",icon_err->message);g_error_free(icon_err);}}
    App*a=g_new0(App,1);
    g_mutex_init(&a->progress_lock);
    a->backend=detect_backend();
    eapt_debug_log("backend selected=%s(%d) is_root=%d",backend_name(a),(int)a->backend,(geteuid()==0));
    a->packages=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,package_free);a->search_folded=g_strdup("");a->pty_master=-1;
    a->filter_mode=F_INSTALLED;
    build_ui(a);
    eapt_debug_log("main UI built window=%p",(void*)a->window);
    gtk_widget_show_all(a->window);
    eapt_debug_widget("MAIN after show_all",a->window);
    eapt_debug_toplevels("MAIN after show_all");
    if(a->backend==BACKEND_NONE){GtkWidget*d=gtk_message_dialog_new(GTK_WINDOW(a->window),GTK_DIALOG_MODAL,GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"No supported package backend was found. Install EDPKG or APT/dpkg.");eapt_debug_watch_window(d,"BACKEND-ERROR");gtk_dialog_run(GTK_DIALOG(d));gtk_widget_destroy(d);}
    else load_local_async(a);
    eapt_debug_log("enter gtk_main");
    gtk_main();
    eapt_debug_log("gtk_main returned");
    eapt_debug_session_end();
    if(a->worker_thread){GThread*t=a->worker_thread;a->worker_thread=NULL;g_thread_join(t);}
    if(a->load_thread){GThread*t=a->load_thread;a->load_thread=NULL;g_thread_join(t);}
    if (a->pulse_id)
        g_source_remove(a->pulse_id);
    if (a->search_debounce_id)
        g_source_remove(a->search_debounce_id);
    if (a->pty_input)
        g_string_free(a->pty_input, TRUE);
    if (a->progress_flush_id)
        g_source_remove(a->progress_flush_id);
    if (a->progress_pending)
        g_string_free(a->progress_pending, TRUE);
    g_mutex_clear(&a->progress_lock);
    if (a->sorted_packages)
        g_ptr_array_free(a->sorted_packages, TRUE);

    g_free(a->search_folded);

    if (a->icon_installed)
        g_object_unref(a->icon_installed);
    if (a->icon_broken)
        g_object_unref(a->icon_broken);
    if (a->icon_upgradable)
        g_object_unref(a->icon_upgradable);

    g_hash_table_destroy(a->packages);
    g_free(a);
    return 0;
}
