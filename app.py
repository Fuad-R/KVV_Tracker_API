import requests
import tkinter as tk
from tkinter import ttk
from collections import defaultdict
from typing import Optional

BASE_URL = "https://kvvapi.fuadserver.uk/api"


def get_stop_id(stop_name: str) -> Optional[str]:
    response = requests.get(
        f"{BASE_URL}/stops/search",
        params={"q": stop_name},
        timeout=10
    )
    response.raise_for_status()

    results = response.json()
    if not results:
        return None

    return results[0]["id"]


def get_stop_departures(stop_id: str) -> list:
    response = requests.get(
        f"{BASE_URL}/stops/{stop_id}",
        timeout=10
    )
    response.raise_for_status()

    return response.json()


class KVVApp(tk.Tk):
    def __init__(self):
        super().__init__()

        self.title("KVV Departures")
        self.geometry("750x500")

        # Input frame
        input_frame = ttk.Frame(self)
        input_frame.pack(fill="x", padx=10, pady=10)

        ttk.Label(input_frame, text="Stop name:").pack(side="left")
        self.stop_entry = ttk.Entry(input_frame, width=30)
        self.stop_entry.pack(side="left", padx=5)

        search_btn = ttk.Button(input_frame, text="Search", command=self.search)
        search_btn.pack(side="left", padx=5)

        # Output area
        self.output = tk.Text(self, wrap="none")
        self.output.pack(fill="both", expand=True, padx=10, pady=10)

        scrollbar = ttk.Scrollbar(self, command=self.output.yview)
        scrollbar.pack(side="right", fill="y")
        self.output.configure(yscrollcommand=scrollbar.set)

    def search(self):
        stop_name = self.stop_entry.get().strip()
        self.output.delete("1.0", tk.END)

        if not stop_name:
            self.output.insert(tk.END, "Please enter a stop name.\n")
            return

        try:
            stop_id = get_stop_id(stop_name)
            if not stop_id:
                self.output.insert(tk.END, "No stop found.\n")
                return

            data = get_stop_departures(stop_id)
            self.display_departures(data)

        except Exception as e:
            self.output.insert(tk.END, f"Error: {e}\n")

    def display_departures(self, stop_data: list):
        platforms = defaultdict(list)

        for entry in stop_data:
            platforms[entry["platform"]].append(entry)

        for platform in sorted(platforms.keys()):
            self.output.insert(tk.END, f"\n=== Platform {platform} ===\n")
            self.output.insert(
                tk.END,
                f"{'Line':<6} {'Destination':<35} {'Live':<5} {'Min'}\n"
            )
            self.output.insert(tk.END, "-" * 60 + "\n")

            departures = sorted(
                platforms[platform],
                key=lambda x: x["minutes_remaining"]
            )

            for d in departures:
                live = "Yes" if d["is_realtime"] else "No"
                self.output.insert(
                    tk.END,
                    f"{d['line']:<6} "
                    f"{d['direction']:<35} "
                    f"{live:<5} "
                    f"{d['minutes_remaining']}m\n"
                )


if __name__ == "__main__":
    app = KVVApp()
    app.mainloop()
