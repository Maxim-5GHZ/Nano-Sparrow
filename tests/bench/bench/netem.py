"""Управление tc netem на egress-интерфейсе runner'а (нужен NET_ADMIN).

netem применяется к eth0 целиком: весь трафик runner->серверы идёт через
«испорченный» канал, что честно ухудшает все серверы одинаково.
"""
import subprocess

_IF = "eth0"


def apply(tc_args):
    """Наложить qdisc netem. Старый (если был) сначала снимается."""
    clear()
    subprocess.run(["tc", "qdisc", "add", "dev", _IF, "root", "netem"] + tc_args,
                   capture_output=True, text=True, check=True)


def clear():
    subprocess.run(["tc", "qdisc", "del", "dev", _IF, "root"],
                   capture_output=True, text=True)


def describe(tc_args):
    if not tc_args:
        return "none"
    return " ".join(tc_args)
