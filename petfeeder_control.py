"""
PetFeeder – Script de linha de comando
========================================
Envia comandos e/ou escuta o status do alimentador IoT via MQTT.

Uso:
    python petfeeder_control.py --command racao
    python petfeeder_control.py --command agua
    python petfeeder_control.py --listen
    python petfeeder_control.py --command racao --listen

    # Alterar agenda de alimentação:
    python petfeeder_control.py --config "{\"h1\":8,\"m1\":0,\"h2\":18,\"m2\":0,\"porcao\":5}"

    # Sincronizar relógio:
    python petfeeder_control.py --sync-rtc
"""

import argparse
import datetime
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt

BROKER       = "broker.emqx.io"
PORT         = 1883
TOPIC_CMD    = "petfeeder/comando"
TOPIC_STATUS = "petfeeder/status"
TOPIC_CONFIG = "petfeeder/config"
TOPIC_RTC    = "petfeeder/relogio"


def make_client(userdata=None):
    c = mqtt.Client(userdata=userdata or {})
    return c


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("[MQTT] Conectado ao broker")
        if userdata.get("listen"):
            client.subscribe(TOPIC_STATUS)
            print(f"[MQTT] Inscrito em {TOPIC_STATUS}")
    else:
        print(f"[MQTT] Falha na conexão (rc={rc})")


def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode("utf-8"))
        print(f"\n[STATUS] {json.dumps(data, indent=2, ensure_ascii=False)}")
    except Exception:
        print(f"[MQTT] Mensagem em {msg.topic}: {msg.payload}")


def wait_connected(client, timeout=5):
    end = time.time() + timeout
    while not client.is_connected() and time.time() < end:
        time.sleep(0.1)
    if not client.is_connected():
        print("[ERRO] Não foi possível conectar ao broker MQTT")
        sys.exit(1)


def publish_once(topic: str, payload: str):
    client = make_client({"listen": False})
    client.on_connect = on_connect
    client.connect(BROKER, PORT)
    client.loop_start()
    wait_connected(client)
    client.publish(topic, payload)
    time.sleep(0.5)
    client.loop_stop()
    client.disconnect()


def listen_forever(stop_event: threading.Event):
    client = make_client({"listen": True})
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER, PORT)
    client.loop_start()
    try:
        while not stop_event.is_set():
            time.sleep(0.1)
    finally:
        client.loop_stop()
        client.disconnect()


def main():
    parser = argparse.ArgumentParser(description="Controle do PetFeeder via MQTT")
    parser.add_argument("--command", choices=["racao", "agua"],
                        help="Envia um comando manual ao alimentador")
    parser.add_argument("--listen", action="store_true",
                        help="Escuta o status em tempo real (petfeeder/status)")
    parser.add_argument("--config", metavar="JSON",
                        help='Envia nova agenda/porção, ex: \'{"h1":8,"m1":0,"h2":18,"m2":0,"porcao":5}\'')
    parser.add_argument("--sync-rtc", action="store_true",
                        help="Sincroniza o RTC com o horário do sistema agora")
    args = parser.parse_args()

    # Sync RTC
    if args.sync_rtc:
        now = datetime.datetime.now()
        payload = json.dumps({
            "ano": now.year, "mes": now.month, "dia": now.day,
            "hora": now.hour, "min": now.minute, "seg": now.second
        })
        print(f"[RTC] Sincronizando: {payload}")
        publish_once(TOPIC_RTC, payload)

    # Send config
    if args.config:
        try:
            parsed = json.loads(args.config)
            payload = json.dumps(parsed)
            print(f"[CONFIG] Enviando: {payload}")
            publish_once(TOPIC_CONFIG, payload)
        except json.JSONDecodeError as e:
            print(f"[ERRO] JSON inválido: {e}")
            sys.exit(1)

    # Listener (background thread)
    stop_event = threading.Event()
    if args.listen:
        t = threading.Thread(target=listen_forever, args=(stop_event,), daemon=True)
        t.start()
        time.sleep(0.5)

    # Manual command
    if args.command:
        print(f"[CMD] Enviando '{args.command}' para {TOPIC_CMD}")
        publish_once(TOPIC_CMD, args.command)

    # Keep running if only listening
    if args.listen:
        try:
            print("Pressione Ctrl+C para parar...")
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n[INFO] Encerrando...")
            stop_event.set()

    if not any([args.command, args.listen, args.config, args.sync_rtc]):
        parser.print_help()


if __name__ == "__main__":
    main()
