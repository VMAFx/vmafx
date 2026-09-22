# Cyclomatic complexity 21 in 43 lines. The ruff template selects E,F,I,N,UP,B,A,C4,T20,SIM
# -- neither C90 (mccabe) nor PLR0912/PLR0915 -- and no Python linter runs in this repo's
# gate at all.
def branchy(count):
    if count > 0:
        count += 1
    if count > 1:
        count += 1
    if count > 2:
        count += 1
    if count > 3:
        count += 1
    if count > 4:
        count += 1
    if count > 5:
        count += 1
    if count > 6:
        count += 1
    if count > 7:
        count += 1
    if count > 8:
        count += 1
    if count > 9:
        count += 1
    if count > 10:
        count += 1
    if count > 11:
        count += 1
    if count > 12:
        count += 1
    if count > 13:
        count += 1
    if count > 14:
        count += 1
    if count > 15:
        count += 1
    if count > 16:
        count += 1
    if count > 17:
        count += 1
    if count > 18:
        count += 1
    if count > 19:
        count += 1
    return count
