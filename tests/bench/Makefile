COMPOSE = docker compose -f docker-compose.yml

.PHONY: bench demo prebuild up down logs clean

# Одна команда для проверяющего: поднять стенд + демо-прогон (WORKERS=1 и 3)
bench:
	./run.sh

demo:
	./run.sh

# Полная матрица (по 15 с на сценарий)
full:
	./run.sh --full

# Предварительная сборка образов (для машины без интернета на конференции)
prebuild:
	./run.sh --build

# Поднять серверы вручную (без прогона): HTTP-порты 18080..18083, TLS 18443..18446
up:
	$(COMPOSE) build sparrow backend
	$(COMPOSE) up -d

down:
	$(COMPOSE) down

logs:
	$(COMPOSE) logs --tail 100 -f

clean:
	rm -rf results/*
