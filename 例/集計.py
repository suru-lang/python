# 例/スクリプト.suru から動かす Python のスクリプトです
import csv
import io
import sys

表 = """名前,点
花子,90
太郎,72
次郎,85
"""

行群 = list(csv.DictReader(io.StringIO(表)))
平均 = sum(int(r["点"]) for r in 行群) / len(行群)
print(f"{len(行群)} 人の平均点は {平均:.1f} 点です")
print("引数:", sys.argv)
