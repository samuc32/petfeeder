import http.server
import socketserver
import os
import sys

PORT = 8080
DIRECTORY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mobile_app")

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

def get_ip():
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

local_ip = get_ip()
url = f"http://{local_ip}:{PORT}"

print("=" * 60)
print("       🐾 PETFEEDER - APP PARA CELULAR INICIADO! 🐾")
print("=" * 60)
print(f"\n👉 No navegador do celular (Chrome/Safari), acesse:")
print(f"   \033[1;32m{url}\033[0m")
print("\n💡 Dica no Celular:")
print("   Toque nos '3 pontinhos' do Chrome (ou 'Compartilhar' no Safari)")
print("   e selecione 'Adicionar a tela inicial' para instalar como App!\n")
print("Pressione Ctrl+C para encerrar o servidor.")
print("=" * 60)

with socketserver.TCPServer(("", PORT), Handler) as httpd:
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nServidor encerrado.")
