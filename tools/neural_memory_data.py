"""Controlled bilingual evidence-retrieval curriculum, not human activity logs.

Families and locations are disjoint across splits. Relevant evidence includes
permission, prohibition AND conditions; negation is never a reason to hide a source.
All labels are constructed before fitting. No metadata is an input to the model.
"""
import hashlib
import json

# topic, two EN actions, two RU actions; the last eight families are withheld.
FAMILIES = [
    ("library", "borrow a book", "print a document", "взять книгу", "напечатать документ"),
    ("laboratory", "use the microscope", "store chemicals", "работать с микроскопом", "хранить реагенты"),
    ("workshop", "operate the drill", "borrow tools", "работать с дрелью", "брать инструменты"),
    ("office", "work remotely", "invite a visitor", "работать удалённо", "пригласить посетителя"),
    ("garden", "water the plants", "pick flowers", "поливать растения", "собирать цветы"),
    ("studio", "record music", "move the lighting", "записывать музыку", "перемещать освещение"),
    ("depot", "charge a vehicle", "park overnight", "заряжать транспорт", "парковаться на ночь"),
    ("school", "use the gym", "join the class", "пользоваться спортзалом", "присоединиться к занятию"),
    ("archive", "photograph manuscripts", "remove folders", "фотографировать рукописи", "выносить папки"),
    ("kitchen", "use the oven", "freeze ingredients", "пользоваться духовкой", "замораживать продукты"),
    ("harbour", "moor a boat", "unload cargo", "швартовать лодку", "разгружать груз"),
    ("observatory", "use the telescope", "enter the dome", "пользоваться телескопом", "входить в купол"),
    ("museum", "touch the exhibits", "bring a camera", "трогать экспонаты", "приносить камеру"),
    ("shelter", "feed the animals", "walk a dog", "кормить животных", "выгуливать собаку"),
    ("theatre", "attend a rehearsal", "enter backstage", "посещать репетицию", "заходить за кулисы"),
    ("camp", "light a fire", "pitch a tent", "разводить огонь", "ставить палатку"),
]
SITES = ["Alder", "Birch", "Cedar", "Dogwood", "Elm", "Fir", "Ginkgo", "Hazel",
         "Iris", "Juniper", "Kestrel", "Linden", "Maple", "Nettle", "Olive", "Pine",
         "Quartz", "Rowan", "Spruce", "Tamarisk", "Umber", "Violet", "Willow", "Xenia",
         "Yarrow", "Zinnia", "Amber", "Beryl", "Coral", "Dahlia", "Ember", "Flint",
         "Garnet", "Heather", "Indigo", "Jasper", "Kiwi", "Lilac", "Marble", "Nectar",
         "Opal", "Pearl", "Robin", "Sage", "Topaz", "Upland", "Velvet", "Wren",
         "Acacia", "Bamboo", "Cypress", "Dune", "Elder", "Fennel", "Glade", "Holly",
         "Ivory", "Jade", "Laurel", "Mallow", "Onyx", "Pebble", "Reed", "Silver"]

def build():
    result = {}
    for split, indices in (("train", range(8)), ("dev", range(8, 12)), ("test", range(12, 16))):
        memories, queries = [], []
        for fi in indices:
            family, en0, en1, ru0, ru1 = FAMILIES[fi]
            for si in range(4):
                site = SITES[4 * fi + si]
                for ai, (en, ru) in enumerate(((en0, ru0), (en1, ru1))):
                    keys = []
                    if si < 3:
                        for state in range(3):
                            key = f"{family}.{site}.{ai}.{state}"
                            lang = (si + ai + state) % 2
                            if lang == 0:
                                forms = [f"At {site}, you may {en}.",
                                         f"At {site}, you must not {en}.",
                                         f"At {site}, you may {en} only with written permission."]
                            else:
                                forms = [f"В {site} разрешено {ru}.",
                                         f"В {site} запрещено {ru}.",
                                         f"В {site} можно {ru} только с письменным разрешением."]
                            memories.append(dict(key=key, text=forms[state], family=family,
                                                 site=site, action=ai, evidence=("permission", "counterevidence", "condition")[state]))
                            keys.append(key)
                    for lang in ("en", "ru"):
                        if split == "train":
                            query = f"Can I {en} at {site}?" if lang == "en" else f"Можно ли {ru} в {site}?"
                        elif split == "dev":
                            query = f"What are the restrictions if I want to {en} at {site}?" if lang == "en" else f"Какие ограничения действуют в {site}, если я хочу {ru}?"
                        else:
                            query = f"I am visiting {site} and plan to {en}. What should I know?" if lang == "en" else f"Я собираюсь в {site} и хочу {ru}. Что мне нужно учесть?"
                        queries.append(dict(text=query, expected=keys, family=family, site=site, action=ai, language=lang))
        result[split] = dict(memories=memories, queries=queries)
    return result

def canonical(x):
    return json.dumps(x, sort_keys=True, ensure_ascii=False, separators=(",", ":"))

def digest(x):
    return hashlib.sha256(canonical(x).encode()).hexdigest()

if __name__ == "__main__":
    data = build()
    print(json.dumps({s: {"memories":len(d["memories"]), "queries":len(d["queries"]), "sha256":digest(d)} for s,d in data.items()}, indent=2))
