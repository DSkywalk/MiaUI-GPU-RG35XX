#!/usr/bin/env python3
import os
import sys
import subprocess
import pygame
import textwrap
import zlib
import glob
import re
import time

# --- CONFIGURACIÓN DE RUTAS ---
LANG_CONF = "/userdata/Profile/language.conf"
DIR_UPDATE = "./"
DEST_DIR = "/"

def get_language():
    """Lee el archivo de configuración y decide el idioma."""
    if os.path.exists(LANG_CONF):
        try:
            with open(LANG_CONF, "r", encoding="utf-8") as f:
                lang = f.read().strip().lower()
                if "es" in lang: return "es"
        except Exception:
            pass
    return "en"

def load_texts(lang):
    texts = {
        "en": {
            "title": "Local System Updater",
            "opt_install": "Install Update",
            "opt_exit": "Exit",
            "inst_title": "UPDATE STATUS:",
            "inst_1": "1. 'update_XXXXXXXX.tar.gz' should exists",
            "inst_2": "   inside the 'Updater.pak' folder.",
            "inst_scroll": "D-Pad Up/Down to Scroll | Button A to continue",
            "status_checking": "Verifying update file, please wait...",
            "status_missing": "--> NO UPDATE FILE FOUND <--",
            "status_multiple": "--> ERROR: MULTIPLE FILES FOUND <--",
            "status_format": "--> ERROR: INVALID FILENAME FORMAT <--",
            "status_corrupt": "--> ERROR: FILE CORRUPTED (BAD CRC) <--",
            "status_ready": "[OK] Update verified (CRC Match). Ready to install.",
            "log_title": "Log: {}",
            "log_start": "=== STARTING UPDATE PROCESS ===\n\n",
            "log_rw": "-> Unlocking filesystem (Remount RW)...\n",
            "log_err_rw": "[ERROR] Failed to unlock filesystem!\n",
            "log_extract": "-> Extracting update (.gz) to root (/). Please wait...\n",
            "log_err_tar": "[ERROR] Extraction failed:\n",
            "log_ro": "-> Locking filesystem (Remount RO)...\n",
            "log_clean": "-> Cleaning temporary update file...\n",
            "log_rename": "-> Disabling Updater App (Renaming to .pak.off)...\n",
            "log_err_rename": "[WARNING] Failed to rename Updater.pak folder.\n",
            "log_success": "\n=== UPDATE COMPLETED SUCCESSFULLY ==="
        },
        "es": {
            "title": "Actualizador de Sistema Local",
            "opt_install": "Instalar Actualización",
            "opt_exit": "Salir",
            "inst_title": "ESTADO DE LA ACTUALIZACIÓN:",
            "inst_1": "1. 'update_XXXXXXXX.tar.gz' debe existir",
            "inst_2": "   dentro de la carpeta 'Updater.pak'",
            "inst_scroll": "D-Pad Arriba/Abajo para Scroll | Botón A para continuar",
            "status_checking": "Verificando archivo, por favor espera...",
            "status_missing": "--> NO SE ENCONTRÓ NINGÚN ARCHIVO <--",
            "status_multiple": "--> ERROR: HAY VARIOS ARCHIVOS <--",
            "status_format": "--> ERROR: FORMATO DE NOMBRE INVÁLIDO <--",
            "status_corrupt": "--> ERROR: ARCHIVO CORRUPTO (MAL CRC) <--",
            "status_ready": "[OK] Archivo verificado (CRC correcto). Listo para instalar.",
            "log_title": "Registro: {}",
            "log_start": "=== INICIANDO PROCESO DE ACTUALIZACIÓN ===\n\n",
            "log_rw": "-> Desbloqueando el sistema de archivos (Remount RW)...\n",
            "log_err_rw": "[ERROR] ¡Fallo al desbloquear el sistema!\n",
            "log_extract": "-> Extrayendo actualización (.gz) en (/). Por favor, espera...\n",
            "log_err_tar": "[ERROR] La extracción ha fallado:\n",
            "log_ro": "-> Bloqueando el sistema de archivos (Remount RO)...\n",
            "log_clean": "-> Limpiando archivo de actualización temporal...\n",
            "log_rename": "-> Desactivando app Updater (Renombrando a .pak.off)...\n",
            "log_err_rename": "[AVISO] No se pudo renombrar la carpeta Updater.pak.\n",
            "log_success": "\n=== ACTUALIZACIÓN COMPLETADA CON ÉXITO ==="
        }
    }
    return texts.get(lang, texts["en"])

# --- LÓGICA DE VALIDACIÓN PREVIA ---

def calculate_crc32(file_path):
    """Calcula el CRC32 de un archivo por bloques."""
    crc = 0
    try:
        with open(file_path, 'rb') as f:
            for chunk in iter(lambda: f.read(8192), b""):
                crc = zlib.crc32(chunk, crc)
        return format(crc & 0xFFFFFFFF, '08x')
    except Exception:
        return None

def verify_update_file():
    """Busca y verifica el archivo antes de mostrar el menú.
       Devuelve una tupla: (estado_string, ruta_archivo_valido)"""
    patron = os.path.join(DIR_UPDATE, "update_*.tar.gz")
    archivos_encontrados = glob.glob(patron)
    
    time.sleep(2)
    
    if not archivos_encontrados:
        return "MISSING", None
    if len(archivos_encontrados) > 1:
        return "MULTIPLE", None
        
    archivo_tar = archivos_encontrados[0]
    nombre_base = os.path.basename(archivo_tar)
    
    # Extraer el CRC del nombre usando Expresiones Regulares
    match = re.match(r"update_([a-zA-Z0-9]+)\.tar\.gz", nombre_base)
    if not match:
        return "FORMAT", None
        
    expected_crc = match.group(1).lower()
    actual_crc = calculate_crc32(archivo_tar)
    
    if actual_crc != expected_crc:
        return "CORRUPT", None
        
    return "READY", archivo_tar

# --- LÓGICA DE ACTUALIZACIÓN ---

def run_update_process(t, archivo_tar):
    """Ejecuta el flujo completo asumiendo que el archivo ya es válido.
       Devuelve una tupla: (texto_log, booleano_exito)"""
    log = t["log_start"]
    
    # 1. Desbloquear sistema (Remount RW)
    log += t["log_rw"]
    if os.system("mount -o remount,rw /") != 0:
        return log + t["log_err_rw"], False

    # 2. Extraer 
    log += t["log_extract"]
    tmp_log = "/tmp/tar_error.log"
    cmd = f"gzip -dc {archivo_tar} | tar -xf - -C {DEST_DIR} > {tmp_log} 2>&1"
    
    if os.system(cmd) != 0:
        err_msg = ""
        if os.path.exists(tmp_log):
            with open(tmp_log, "r") as f: err_msg = f.read()
            os.remove(tmp_log)
        os.system("mount -o remount,ro /")
        return log + t["log_err_tar"] + err_msg, False

    # 3. Bloquear sistema (Remount RO)
    log += t["log_ro"]
    os.system("mount -o remount,ro /")

    # 4. Limpieza del archivo de actualización (SOLO si no hubo errores previos)
    log += t["log_clean"]
    try:
        os.remove(archivo_tar)
    except:
        pass

    # 5. Remove app just renaming
    current_dir = os.path.dirname(os.path.abspath(__file__))
    if current_dir.endswith("Updater.pak"):
        log += t["log_rename"]
        try:
            folder_parent = os.path.dirname(current_dir)
            folder_name = os.path.basename(current_dir)

            # /userdata/.../.off.Updater.pak
            new_path = os.path.join(folder_parent, ".off." + folder_name)
            
            os.rename(current_dir, new_path)
        except Exception:
            log += t["log_err_rename"]

    log += t["log_success"]
    return log, True

# --- INTERFAZ PYGAME ---

def quit_pygame(screen):
    for _ in range(0, 10):
        screen.fill((30, 30, 30))
        pygame.display.flip()
    pygame.quit()
    os.system("cat /dev/zero > /dev/fb0 2>/dev/null")
    sys.exit()

def init_pygame():
    pygame.init()
    pygame.joystick.init()
    for i in range(pygame.joystick.get_count()):
        pygame.joystick.Joystick(i).init()

    screen = pygame.display.set_mode((640, 480))
    pygame.display.set_caption("Local Updater")
    
    font_title = pygame.font.Font("/opt/minui/res/BPreplayBold-unhinted.otf", 29, bold=True)
    font_list = pygame.font.Font("/opt/minui/res/BPreplayBold-unhinted.otf", 25)
    font_info = pygame.font.SysFont("Arial", 27) 
    font_log = pygame.font.SysFont("Courier", 20) 
    
    return screen, font_title, font_list, font_info, font_log

def draw_checking_screen(screen, font_title, font_list, t):
    """Muestra una pantalla de carga mientras se calcula el CRC."""
    screen.fill((30, 30, 30))
    title_surf = font_title.render(t["title"], True, (255, 255, 255))
    screen.blit(title_surf, (20, 20))
    
    msg_surf = font_list.render(t["status_checking"], True, (100, 200, 255))
    msg_rect = msg_surf.get_rect(center=(320, 240))
    screen.blit(msg_surf, msg_rect)
    pygame.display.flip()

def show_log_viewer(screen, font_title, font_log, title, log_text, t):
    clock = pygame.time.Clock()
    raw_lines = log_text.split('\n')
    wrapped_lines = []
    
    for line in raw_lines:
        if not line:
            wrapped_lines.append("")
        else:
            wrapped_lines.extend(textwrap.wrap(line, width=65))
            
    max_visible_lines = 15
    scroll_offset = 0

    while True:
        screen.fill((20, 20, 25))
        
        title_surf = font_title.render(title, True, (255, 255, 255))
        screen.blit(title_surf, (20, 20))
        
        y_pos = 70
        end_idx = min(scroll_offset + max_visible_lines, len(wrapped_lines))
        
        for i in range(scroll_offset, end_idx):
            line_surf = font_log.render(wrapped_lines[i], True, (150, 255, 150))
            screen.blit(line_surf, (20, y_pos))
            y_pos += 22 
            
        footer_bg = pygame.Rect(0, 430, 640, 50)
        pygame.draw.rect(screen, (40, 40, 50), footer_bg)
        
        footer_surf = font_log.render(t["inst_scroll"], True, (255, 255, 150))
        screen.blit(footer_surf, (20, 445))

        pygame.display.flip()

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                quit_pygame(screen)
            elif event.type == pygame.JOYBUTTONDOWN:
                if event.button == 0 or event.button == 10: 
                    pygame.time.delay(200)
                    pygame.event.clear()
                    return 
                elif event.button == 13: 
                    if scroll_offset > 0: scroll_offset -= 1
                elif event.button == 14: 
                    if scroll_offset < len(wrapped_lines) - max_visible_lines: scroll_offset += 1
                elif event.button == 15: 
                    scroll_offset = max(0, scroll_offset - max_visible_lines)
                elif event.button == 16: 
                    scroll_offset = min(max(0, len(wrapped_lines) - max_visible_lines), scroll_offset + max_visible_lines)
        clock.tick(30)

def draw_instructions(screen, font_list, font_info, t, file_status):
    box_rect = pygame.Rect(10, 300, 620, 170)
    pygame.draw.rect(screen, (45, 45, 50), box_rect)
    pygame.draw.rect(screen, (80, 80, 90), box_rect, 2)

    title_surf = font_list.render(t["inst_title"], True, (100, 255, 100))
    screen.blit(title_surf, (25, 310))

    # Definimos el mensaje dinámico según el estado que devolvió verify_update_file()
    status_msg = ""
    status_color = (255, 100, 100) # Rojo por defecto para errores
    
    if file_status == "READY":
        status_msg = t["status_ready"]
        status_color = (100, 255, 100) # Verde si está todo bien
    elif file_status == "MISSING":
        status_msg = t["status_missing"]
    elif file_status == "MULTIPLE":
        status_msg = t["status_multiple"]
    elif file_status == "FORMAT":
        status_msg = t["status_format"]
    elif file_status == "CORRUPT":
        status_msg = t["status_corrupt"]

    lines = [t["inst_1"], t["inst_2"], "", status_msg]

    y_pos = 350
    for i, line in enumerate(lines):
        # La última línea (la 3) es el estado dinámico y lleva su propio color
        color = status_color if i == 3 else (200, 200, 200)
        line_surf = font_info.render(line, True, color)
        screen.blit(line_surf, (25, y_pos))
        y_pos += 26

def run_menu(screen, font_title, font_list, font_info, title, options, t, file_status, default_index=0):
    selected = default_index
    if selected >= len(options):
        selected = 0
        
    clock = pygame.time.Clock()

    while True:
        screen.fill((30, 30, 30))
        
        title_surf = font_title.render(title, True, (255, 255, 255))
        screen.blit(title_surf, (20, 20))
        
        y_offset = 80
        for i, option in enumerate(options):
            color = (50, 255, 50) if i == selected else (200, 200, 200)
            prefix = "> " if i == selected else "  "
            text_surf = font_list.render(f"{prefix}{option}", True, color)
            screen.blit(text_surf, (20, y_offset))
            y_offset += 45

        draw_instructions(screen, font_list, font_info, t, file_status)
        pygame.display.flip()

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                quit_pygame(screen)
            elif event.type == pygame.JOYBUTTONDOWN:
                if event.button == 0: 
                    pygame.time.delay(200) 
                    pygame.event.clear()
                    return selected
                if event.button == 13: selected = (selected - 1) % len(options)
                elif event.button == 14: selected = (selected + 1) % len(options)
                elif event.button == 10: quit_pygame(screen)

        clock.tick(30)

# --- LÓGICA PRINCIPAL ---

def main():
    lang = get_language()
    t = load_texts(lang)
    
    screen, font_title, font_list, font_info, font_log = init_pygame()

    # 1. Dibujamos la pantalla de "Cargando..." y calculamos el CRC
    draw_checking_screen(screen, font_title, font_list, t)
    file_status, valid_file_path = verify_update_file()

    # 2. Entramos al bucle principal con el resultado de la comprobación
    while True:
        if file_status == "READY":
            menu_options = [t["opt_install"], t["opt_exit"]]
        else:
            menu_options = [t["opt_exit"]]
            
        selected_idx = run_menu(screen, font_title, font_list, font_info, t["title"], menu_options, t, file_status)
        selected_action = menu_options[selected_idx]

        if selected_action == t["opt_exit"]:
            break
            
        if selected_action == t["opt_install"]:
            # Pasamos directamente la ruta válida que obtuvimos al principio
            log_resultado, exito = run_update_process(t, valid_file_path)
            log_title = t["log_title"].format(selected_action)
            show_log_viewer(screen, font_title, font_log, log_title, log_resultado, t)
            
            # Si todo ha ido bien (borrado y renombrado .off), salimos al frontend
            if exito:
                break

    quit_pygame(screen)

if __name__ == "__main__":
    main()
