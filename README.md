# Wskrzeszanie smoków, Inc.
Projekt wykonany w ramach przedmiotu Programowanie Rozproszone.

## Kompilacja
```bash
$ make
```

## Uruchomienie
Liczba procesów, dla której uruchamiany jest algorytm musi zgadzać się z konfiguracją, dla której został skompilowany. W przeciwnym wypadku program zakończy się i poinformuje o wymaganej liczbie procesów.

```bash
$ mpirun -n LICZBA_PROCESÓW smoki
```

## Oryginalny opis zadania
Opis ze strony dr inż. Arkadiusza Danileckiego:
"Ubić smoka może byle idiota. Za to wskrzesić... do tego potrzeba profesjonalisty. Są dwa rodzaje procesów: jeden generuje co pewien czas zlecenie. O zlecenie ubiegają się profesjonaliści o jednej z trzech możliwych specjalizacji (głowa, ogon, tułów). Do realizacji zlecenia potrzeba trzech profesjonalistów o różnych specjalizacjach - należy zapewnić, by nie doszło do zakleszczeń! Dodatkowo, profesjonaliści muszą wypełnić robotę papierkową (robi to jeden z nich) przy jednym z b biurek w gildii Wskrzeszania Smoków. Następnie profesjonaliści zdobywają dostęp do jednego z s szkieletów smoków i rozpoczynają wskrzeszanie. Należy zapewnić, by profesjonaliści dzielili się w miarę równo pracą."

## Algorytm
Agorytm symulujący żądaną sytuację oparty jest o podział zasobów rozróżnialnych na pierścienie oraz utworzenie rozproszonych semaforów dla zasobów nierozróżnialnych.
- Grupy procesów odpowiedzialne za nekromantów specjalizujących się w głowach, ciałach i ogonach podzielone zostają na trzy pierścienie
- Proces rozgłaszający zadania wysyła je do nekromantów głów
- Proces, który posiada żeton przyjmuje zadanie i przekazuje żeton dalej, następnie wysyła żądania do pierścieni ciał i ogonów
- W każdym z tych pierścieni proces, który posiada żeton odsyła potwierdzenie przyjęcia zadania
- Po otrzymaniu potwierdzeń, nekromanta głowy ubiega się o przydział biurka (nierozróżnialnego zasobu) przy pomocy wariantu algorytmu Ricarta-Agrawali
- Następnie analogicznie ubiega się o przydział szkieletu
- Po wskrzeszeniu smoka, wysyła wiadomości o ukończeniu zadania do procesów nekromanty ciała i ogona, oraz zwalnia zasoby (biurko i szkielet)
- W każdym pierścieniu zwolniony proces rozgłasza informację o zakończeniu zadania
